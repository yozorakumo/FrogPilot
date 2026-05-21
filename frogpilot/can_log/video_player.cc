// video_player.cc - HEVC映像をVisionIPCで配信するCAN Playback用バイナリ
//
// can_player.pyとParams経由で同期し、fcamera.hevcをFrameReaderで
// デコードしてVisionIpcServerでUIに配信する。
//
// 使用例:
//   video_player /data/media/0/realdata/2026-05-19--14-30-25--33243391ae
//   video_player /data/media/0/realdata/2026-05-19--14-30-25--33243391ae --loop

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <csignal>
#include <cstdarg>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "common/params.h"
#include "common/util.h"
#include "msgq/visionipc/visionipc_server.h"
#include "system/camerad/cameras/camera_common.h"
#include "tools/replay/framereader.h"
#include "tools/replay/util.h"

#ifdef QCOM2
#include "third_party/linux/include/msm_media_info.h"
#endif

namespace fs = std::filesystem;

// 定数
static const int FPS = 20;
static const int SEGMENT_SEC = 60;
static const int BUFFER_COUNT = 40;
static const int PRELOAD_THRESHOLD = 5 * FPS;  // Pre-load next segment when within 5 seconds of end
static std::atomic<bool> g_exit{false};

// シグナルハンドラ
static void on_signal(int sig) { g_exit = true; }

// --- セグメント探索 ---
// can_player.pyのdiscover_segments()と同じロジック
// フラット構造: /data/media/0/realdata/<route>--<dongle>--<segment>/fcamera.hevc

static std::vector<std::string> discover_segments(const std::string &route_path) {
  fs::path dir(route_path);
  if (!fs::is_directory(dir)) return {};

  std::vector<std::string> result;
  std::string name = dir.filename().string();
  auto pos = name.rfind("--");

  if (pos != std::string::npos) {
    std::string suffix = name.substr(pos + 2);
    bool is_seg = !suffix.empty() && std::all_of(suffix.begin(), suffix.end(), ::isdigit);
    if (is_seg) {
      // フラット構造: 兄弟セグメントを探索
      std::string prefix = name.substr(0, pos);
      fs::path parent = dir.parent_path();
      for (const auto &e : fs::directory_iterator(parent)) {
        if (e.is_directory()) {
          std::string entry_name = e.path().filename().string();
          if (entry_name.find(prefix + "--") == 0) {
            if (fs::exists(e.path() / "fcamera.hevc")) {
              result.push_back(e.path().string());
            }
          }
        }
      }
    }
  }

  if (result.empty()) {
    // 単一セグメントまたはネスト構造
    if (fs::exists(dir / "fcamera.hevc")) {
      result.push_back(dir.string());
    }
    for (const auto &e : fs::directory_iterator(dir)) {
      if (e.is_directory() && fs::exists(e.path() / "fcamera.hevc")) {
        result.push_back(e.path().string());
      }
    }
  }

  // セグメント番号でソート
  auto seg_num = [](const std::string &p) -> int {
    std::string n = fs::path(p).filename().string();
    auto d = n.rfind("--");
    if (d != std::string::npos) {
      try { return std::stoi(n.substr(d + 2)); } catch (...) {}
    }
    return 0;
  };
  std::sort(result.begin(), result.end(), [&](const auto &a, const auto &b) {
    return seg_num(a) < seg_num(b);
  });

  return result;
}

// --- NV12バッファサイズ計算 ---
// QCOM2ではVenusハードウェアのアライメントが必要

static std::tuple<size_t, size_t, size_t> calc_nv12_info(int w, int h) {
#ifdef QCOM2
  int stride = VENUS_Y_STRIDE(COLOR_FMT_NV12, w);
  int scanlines = VENUS_Y_SCANLINES(COLOR_FMT_NV12, h);
  size_t buf_size = static_cast<size_t>(2346) * stride;
  return {static_cast<size_t>(stride), static_cast<size_t>(scanlines), buf_size};
#else
  size_t stride = static_cast<size_t>(w);
  size_t scanlines = static_cast<size_t>(h);
  size_t buf_size = static_cast<size_t>(w) * h * 3 / 2;
  return {stride, scanlines, buf_size};
#endif
}

// --- メイン ---

int main(int argc, char *argv[]) {
  if (argc < 2) {
    printf("Usage: video_player <route_path> [--loop]\n");
    printf("  route_path: Route directory with fcamera.hevc segments\n");
    printf("  --loop:     Loop playback\n");
    return 1;
  }

  std::string route_path = argv[1];
  bool do_loop = false;
  for (int i = 2; i < argc; i++) {
    if (std::string(argv[i]) == "--loop") do_loop = true;
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  // セグメント探索
  auto segments = discover_segments(route_path);
  if (segments.empty()) {
    fprintf(stderr, "[video_player] No segments with fcamera.hevc found in %s\n", route_path.c_str());
    return 1;
  }
  fprintf(stderr, "[video_player] Found %zu segments\n", segments.size());

  // 古いVisionIPCソケットをクリーンアップ (cameradとの競合回避)
  {
    std::string prefix;
    if (char *p = std::getenv("OPENPILOT_PREFIX")) {
      prefix = std::string(p) + "_";
    }
    std::string sock_path = "/tmp/" + prefix + "visionipc_camerad";
    unlink(sock_path.c_str());
  }

  // メインループ変数
  Params params;
  std::unique_ptr<VisionIpcServer> vipc;
  std::unique_ptr<FrameReader> reader;
  std::unique_ptr<FrameReader> next_reader;  // Pre-loaded next segment
  std::thread cache_thread;                  // Background frame cache thread
  int cur_seg = -1;
  int last_frame = -1;
  int vipc_w = 0, vipc_h = 0;
  size_t frames_sent = 0;
  bool vipc_ready = false;

  // 時間ベース同期の変数（Params読み取りを200ms間隔に削減し、
  // 間は時間補間でフレームを送信してスムーズな20FPSを実現）
  double synced_position = 0.0;
  double synced_speed = 1.0;
  bool synced_paused = false;
  auto last_params_read = std::chrono::steady_clock::now();
  auto last_frame_time = std::chrono::steady_clock::now();
  static constexpr double PARAMS_READ_INTERVAL = 0.1;  // Params読み取り間隔（秒）
  static constexpr double FRAME_INTERVAL = 1.0 / FPS;  // フレーム間隔（秒）

  fprintf(stderr, "[video_player] Waiting for CAN playback to start...\n");

  while (!g_exit) {
    // CAN_PLAYBACKが有効か確認
    if (!params.getBool("CAN_PLAYBACK")) {
      if (frames_sent > 0) {
        fprintf(stderr, "[video_player] CAN_PLAYBACK disabled, exiting\n");
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      continue;
    }

    auto now = std::chrono::steady_clock::now();
    double time_since_read = std::chrono::duration<double>(now - last_params_read).count();

    // Paramsから定期的に状態を読み取り（200ms間隔）
    // 毎フレームParamsを読むとファイルシステムI/OでFPSが低下するため
    if (time_since_read >= PARAMS_READ_INTERVAL) {
      std::string pos_str = params.get("CanPlaybackPosition");
      if (!pos_str.empty()) {
        try { synced_position = std::stod(pos_str); } catch (...) {}
      }

      std::string speed_str = params.get("CanPlaybackSpeed");
      if (!speed_str.empty()) {
        try { synced_speed = std::stod(speed_str); } catch (...) {}
      }

      std::string playing_str = params.get("CanPlaybackPlaying");
      synced_paused = (playing_str == "0");

      last_params_read = now;
      time_since_read = 0.0;
    }

    // 一時停止中はフレームを送信しない
    if (synced_paused) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }

    // 時間補間で現在位置を計算（Params読み取り間の補間）
    double current_pos = synced_position + time_since_read * synced_speed;

    // フレームタイミング制御（20FPS）
    double time_since_frame = std::chrono::duration<double>(now - last_frame_time).count();
    if (time_since_frame < FRAME_INTERVAL * 0.9) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }

    // フレームインデックス計算 (20fps)
    int total_frame = static_cast<int>(current_pos * FPS);
    if (total_frame < 0) total_frame = 0;

    // 既に送信済みのフレームはスキップ
    if (total_frame == last_frame) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }

    // セグメントとセグメント内フレームインデックス
    int seg = total_frame / (SEGMENT_SEC * FPS);
    int frame_in_seg = total_frame % (SEGMENT_SEC * FPS);

    // セグメント範囲チェック
    if (seg >= static_cast<int>(segments.size())) {
      if (do_loop) {
        last_frame = -1;
        cur_seg = -1;
        reader.reset();
        synced_position = 0.0;
        last_params_read = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }

    // セグメント切替え
    if (seg != cur_seg) {
      // Stop previous cache thread
      if (reader) reader->cache_abort_ = true;
      if (cache_thread.joinable()) cache_thread.join();

      // Check if we already pre-loaded this segment
      if (next_reader && seg == cur_seg + 1) {
        reader = std::move(next_reader);
        next_reader.reset();
        fprintf(stderr, "[video_player] Segment %d: using pre-loaded segment\n", seg);
      } else {
        std::string hevc = (fs::path(segments[seg]) / "fcamera.hevc").string();
        auto new_reader = std::make_unique<FrameReader>();
        if (!new_reader->loadFromFile(RoadCam, hevc, false)) {
          fprintf(stderr, "[video_player] Failed to load segment %d: %s\n", seg, hevc.c_str());
          cur_seg = seg;
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
          continue;
        }
        fprintf(stderr, "[video_player] Segment %d: %zu frames, %dx%d\n",
                seg, new_reader->getFrameCount(), new_reader->width, new_reader->height);
        reader = std::move(new_reader);
      }

      // Enable frame cache and start background pre-caching
      if (reader) {
        reader->setCacheSize(std::min<size_t>(reader->getFrameCount(), 600));
        reader->cache_abort_ = false;
        FrameReader *cr = reader.get();
        cache_thread = std::thread([cr]() {
          cr->preCache(0, cr->getFrameCount());
        });
        fprintf(stderr, "[video_player] Frame cache enabled for segment %d (max %zu frames)\n",
                seg, std::min<size_t>(reader->getFrameCount(), 600));
      }

      cur_seg = seg;
    }

    // Pre-load next segment when approaching end of current segment
    if (!next_reader && seg + 1 < static_cast<int>(segments.size())) {
      int frames_in_seg = reader ? static_cast<int>(reader->getFrameCount()) : (SEGMENT_SEC * FPS);
      int frames_remaining = frames_in_seg - frame_in_seg;
      if (frames_remaining < PRELOAD_THRESHOLD) {
        int next_seg = seg + 1;
        std::string next_hevc = (fs::path(segments[next_seg]) / "fcamera.hevc").string();
        auto pre_reader = std::make_unique<FrameReader>();
        if (pre_reader->loadFromFile(RoadCam, next_hevc, false)) {
          fprintf(stderr, "[video_player] Pre-loaded segment %d: %zu frames\n",
                  next_seg, pre_reader->getFrameCount());
          next_reader = std::move(pre_reader);
        }
      }
    }

    if (!reader) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }

    // フレーム範囲チェック
    if (frame_in_seg >= static_cast<int>(reader->getFrameCount())) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }

    // VisionIPC初期化（初回または解像度変更時）
    if (!vipc_ready || reader->width != vipc_w || reader->height != vipc_h) {
      vipc_w = reader->width;
      vipc_h = reader->height;

      vipc.reset();

      vipc = std::make_unique<VisionIpcServer>("camerad");
      auto [stride, scanlines, buf_size] = calc_nv12_info(vipc_w, vipc_h);
      vipc->create_buffers_with_sizes(VISION_STREAM_ROAD, BUFFER_COUNT, false,
                                      vipc_w, vipc_h, buf_size, stride, stride * scanlines);
      vipc->start_listener();
      vipc_ready = true;
      fprintf(stderr, "[video_player] VisionIPC started: %dx%d, stride=%zu, buf_size=%zu\n",
              vipc_w, vipc_h, stride, buf_size);
    }

    // バッファ取得＆フレームデコード
    VisionBuf *buf = vipc->get_buffer(VISION_STREAM_ROAD);
    if (!buf) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }

    if (reader->get(frame_in_seg, buf)) {
      VisionIpcBufExtra extra = {};
      extra.frame_id = static_cast<uint64_t>(total_frame);
      extra.timestamp_sof = static_cast<uint64_t>(current_pos * 1e9);
      extra.timestamp_eof = static_cast<uint64_t>((current_pos + FRAME_INTERVAL) * 1e9);
      vipc->send(buf, &extra, false);
      frames_sent++;
      last_frame = total_frame;
      last_frame_time = std::chrono::steady_clock::now();  // Update after actual send
    }
  }

  // Stop cache thread
  if (reader) reader->cache_abort_ = true;
  if (cache_thread.joinable()) cache_thread.join();

  fprintf(stderr, "[video_player] Exiting. Sent %zu frames total.\n", frames_sent);

  // VisionIPCサーバーのクリーンアップ
  // (unique_ptrのデストラクタがリスナースレッドの終了を待機)
  vipc.reset();

  return 0;
}