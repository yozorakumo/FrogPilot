// video_player.cc - HEVC映像をVision IPCで配信するCAN Playback用バイナリ
//
// can_player.pyとParams経由で同期し、fcamera.hevcをFrameReaderで
// デコードしてVisionIpcServerでUIに配信する。
//
// 事前デコードモード: 再生開始前に全セグメントの全フレームをデコードして
// メモリにキャッシュする。再生時はキャッシュからコピーするのみで高パフォーマンス。
// 進捗はrlog読み込み(0-50%)とフレームデコード(50-100%)を統合して表示。
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
static std::atomic<bool> g_exit{false};

// シグナルハンドラ
static void on_signal(int sig) { g_exit = true; }

// --- 事前デコード用キャッシュ ---
struct CachedFrame {
  std::vector<uint8_t> y_data;
  std::vector<uint8_t> uv_data;
  int y_stride;
  int uv_stride;
  int width;
  int height;
};

struct CachedSegment {
  std::vector<CachedFrame> frames;
};

static std::vector<CachedSegment> g_cached_segments;

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

// --- 全フレーム事前デコード ---
// 全セグメントのフレームをシーケンシャルにデコードしてメモリにキャッシュする。
// 進捗はCanPlaybackLoadingProgressに50-100%の範囲で書き込む。
// can_player.pyのrlog読み込み(0-50%)と統合して単一のパーセンテージで表示。

static void predecodeAllFrames(const std::vector<std::string> &segments, Params &params) {
  g_cached_segments.clear();
  g_cached_segments.resize(segments.size());

  // セグメントごとにFrameReaderを作成してフレーム数を計上
  size_t total_frames = 0;
  size_t decoded_frames = 0;

  std::vector<std::unique_ptr<FrameReader>> readers(segments.size());
  for (size_t i = 0; i < segments.size(); i++) {
    std::string hevc = (fs::path(segments[i]) / "fcamera.hevc").string();
    auto reader = std::make_unique<FrameReader>();
    if (!reader->loadFromFile(RoadCam, hevc, true)) {
      fprintf(stderr, "[video_player] Failed to load segment %zu: %s\n", i, hevc.c_str());
      continue;
    }
    total_frames += reader->getFrameCount();
    fprintf(stderr, "[video_player] Segment %zu: %zu frames, %dx%d\n",
            i, reader->getFrameCount(), reader->width, reader->height);
    readers[i] = std::move(reader);
  }

  if (total_frames == 0) {
    params.put("CanPlaybackLoadingProgress", "100");
    return;
  }

  fprintf(stderr, "[video_player] Decoding %zu total frames...\n", total_frames);

  // 全フレームをシーケンシャルにデコード（シーク不要で高速）
  for (size_t seg = 0; seg < readers.size(); seg++) {
    if (!readers[seg] || g_exit) continue;

    auto &reader = readers[seg];
    size_t frame_count = reader->getFrameCount();
    g_cached_segments[seg].frames.resize(frame_count);

    // 一時VisionBufを準備（デコード出力先）
    size_t y_size = reader->width * reader->height;
    size_t uv_size = reader->width * (reader->height / 2);
    std::vector<uint8_t> y_tmp(y_size);
    std::vector<uint8_t> uv_tmp(uv_size);

    for (size_t f = 0; f < frame_count && !g_exit; f++) {
      VisionBuf tmp_buf = {};
      tmp_buf.y = y_tmp.data();
      tmp_buf.uv = uv_tmp.data();
      tmp_buf.stride = reader->width;
      tmp_buf.width = reader->width;
      tmp_buf.height = reader->height;

      if (reader->get(static_cast<int>(f), &tmp_buf)) {
        CachedFrame &cf = g_cached_segments[seg].frames[f];
        cf.width = reader->width;
        cf.height = reader->height;
        cf.y_stride = reader->width;
        cf.uv_stride = reader->width;
        cf.y_data.assign(tmp_buf.y, tmp_buf.y + y_size);
        cf.uv_data.assign(tmp_buf.uv, tmp_buf.uv + uv_size);
      }

      decoded_frames++;
      // 進捗: 50% + (decoded / total) * 50%
      int progress = 50 + static_cast<int>((decoded_frames * 50) / total_frames);
      if (progress > 99) progress = 99;  // 100%は完了時のみ
      params.put("CanPlaybackLoadingProgress", std::to_string(progress));
    }
  }

  params.put("CanPlaybackLoadingProgress", "100");
  fprintf(stderr, "[video_player] Pre-decode complete: %zu frames cached\n", decoded_frames);
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

  Params params;

  // rlog読み込み完了を待機 (can_playerがCanPlaybackLoadingProgressを50%に設定するまで)
  fprintf(stderr, "[video_player] Waiting for rlog loading to complete...\n");
  auto wait_start = std::chrono::steady_clock::now();
  while (!g_exit) {
    if (!params.getBool("CAN_PLAYBACK")) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      continue;
    }
    std::string prog = params.get("CanPlaybackLoadingProgress");
    int p = 0;
    if (!prog.empty()) {
      try { p = std::stoi(prog); } catch (...) {}
    }
    if (p >= 50) break;

    // タイムアウト: 5分
    double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - wait_start).count();
    if (elapsed > 300.0) {
      fprintf(stderr, "[video_player] Timeout waiting for rlog loading, starting decode anyway\n");
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  if (g_exit) return 0;

  // 全フレーム事前デコード (進捗: 50-100%)
  predecodeAllFrames(segments, params);
  if (g_exit) return 0;

  // 最初の有効セグメントから解像度を取得
  int vipc_w = 0, vipc_h = 0;
  for (const auto &cs : g_cached_segments) {
    if (!cs.frames.empty()) {
      vipc_w = cs.frames[0].width;
      vipc_h = cs.frames[0].height;
      break;
    }
  }

  if (vipc_w == 0 || vipc_h == 0) {
    fprintf(stderr, "[video_player] No valid frames found\n");
    return 1;
  }

  // 古いVisionIPCソケットをクリーンアップ (cameradとの競合回避)
  {
    std::string prefix;
    if (char *p = std::getenv("OPENPILOT_PREFIX")) {
      prefix = std::string(p) + "_";
    }
    std::string sock_path = "/tmp/" + prefix + "visionipc_camerad";
    unlink(sock_path.c_str());
  }

  // VisionIPC初期化
  auto vipc = std::make_unique<VisionIpcServer>("camerad");
  auto [stride, scanlines, buf_size] = calc_nv12_info(vipc_w, vipc_h);
  vipc->create_buffers_with_sizes(VISION_STREAM_ROAD, BUFFER_COUNT, false,
                                  vipc_w, vipc_h, buf_size, stride, stride * scanlines);
  vipc->start_listener();
  fprintf(stderr, "[video_player] VisionIPC started: %dx%d, stride=%zu, buf_size=%zu\n",
          vipc_w, vipc_h, stride, buf_size);

  // 再生開始を待機 (can_playerがCanPlaybackPlaying=1を設定するまで)
  fprintf(stderr, "[video_player] Waiting for playback to start...\n");
  while (!g_exit) {
    if (!params.getBool("CAN_PLAYBACK")) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      continue;
    }
    std::string playing = params.get("CanPlaybackPlaying");
    if (playing == "1") break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (g_exit) return 0;

  // メイン再生ループ - キャッシュからフレームを送信
  int last_frame = -1;
  size_t frames_sent = 0;
  double synced_position = 0.0;
  double synced_speed = 1.0;
  bool synced_paused = false;
  auto last_params_read = std::chrono::steady_clock::now();
  auto last_frame_time = std::chrono::steady_clock::now();
  static constexpr double PARAMS_READ_INTERVAL = 0.1;  // Params読み取り間隔（秒）
  static constexpr double FRAME_INTERVAL = 1.0 / FPS;  // フレーム間隔（秒）

  fprintf(stderr, "[video_player] Playback started\n");

  while (!g_exit) {
    // CAN_PLAYBACKが有効か確認
    if (!params.getBool("CAN_PLAYBACK")) {
      fprintf(stderr, "[video_player] CAN_PLAYBACK disabled, exiting\n");
      break;
    }

    auto now = std::chrono::steady_clock::now();
    double time_since_read = std::chrono::duration<double>(now - last_params_read).count();

    // Paramsから定期的に状態を読み取り
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

    // 時間補間で現在位置を計算
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
    if (seg >= static_cast<int>(g_cached_segments.size())) {
      // ループ時: can_playerが位置をリセットするのを待つ
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      last_frame = -1;
      continue;
    }

    // キャッシュからフレームを取得
    auto &cached_seg = g_cached_segments[seg];
    if (frame_in_seg >= static_cast<int>(cached_seg.frames.size())) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }

    CachedFrame &frame = cached_seg.frames[frame_in_seg];
    if (frame.y_data.empty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }

    // VisionIPCバッファを取得
    VisionBuf *buf = vipc->get_buffer(VISION_STREAM_ROAD);
    if (!buf) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }

    // キャッシュからVisionIPCバッファにコピー
    memcpy(buf->y, frame.y_data.data(), frame.y_data.size());
    memcpy(buf->uv, frame.uv_data.data(), frame.uv_data.size());

    VisionIpcBufExtra extra = {};
    extra.frame_id = static_cast<uint64_t>(total_frame);
    extra.timestamp_sof = static_cast<uint64_t>(current_pos * 1e9);
    extra.timestamp_eof = static_cast<uint64_t>((current_pos + FRAME_INTERVAL) * 1e9);
    vipc->send(buf, &extra, false);
    frames_sent++;
    last_frame = total_frame;
    last_frame_time = std::chrono::steady_clock::now();
  }

  fprintf(stderr, "[video_player] Exiting. Sent %zu frames total.\n", frames_sent);

  // VisionIPCサーバーのクリーンアップ
  vipc.reset();

  return 0;
}