// video_player.cc - HEVC映像をVision IPCで配信するCAN Playback用バイナリ
//
// can_player.pyとParams経由で同期し、fcamera.hevcをFrameReaderで
// デコードしてVisionIpcServerでUIに配信する。
//
// プリデコード非同期モード:
// 別スレッドでフレームを事前デコードしてキューに入れ、
// メインスレッドはキューから取り出してVisionIPCで配信する。
// VisionIpcServerはリングバッファ方式のため、send()しなかった
// バッファは次のget_buffer()で上書きされる（明示的な解放不要）。
//
// 使用例:
//   video_player /data/media/0/realdata/2026-05-19--14-30-25--33243391ae
//   video_player /data/media/0/realdata/2026-05-19--14-30-25--33243391ae --loop

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <queue>
#include <csignal>
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
static const int PREDECODE_QUEUE_SIZE = 3;  // 先読みフレーム数
static std::atomic<bool> g_exit{false};

// --- プリデコードフレームバッファ ---
struct DecodedFrame {
  int frame_id = -1;
  VisionBuf *vipc_buf = nullptr;
  bool valid = false;
};

static std::queue<DecodedFrame> g_decoded_queue;
static std::mutex g_queue_mutex;
static std::condition_variable g_queue_cv;
static constexpr int MAX_QUEUE_SIZE = PREDECODE_QUEUE_SIZE;

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

  Params params;

  // 前回の実行からの古いParamsをリセット
  params.put("CanPlaybackPlaying", "0");
  params.put("CanPlaybackPosition", "0");
  fprintf(stderr, "[video_player] Reset playback params\n");

  // 全セグメントのFrameReaderを作成
  // no_hw_decoder=false → V4L2 HW デコーダー（Venus）を優先使用
  std::vector<std::unique_ptr<FrameReader>> readers(segments.size());
  for (size_t i = 0; i < segments.size(); i++) {
    std::string hevc = (fs::path(segments[i]) / "fcamera.hevc").string();
    auto reader = std::make_unique<FrameReader>();
    // no_hw_decoder=false → HWデコード優先（Venus V4L2 ION、失敗時はCPU自動フォールバック）
    if (!reader->loadFromFile(RoadCam, hevc, false)) {
      fprintf(stderr, "[video_player] Failed to load segment %zu: %s\n", i, hevc.c_str());
      continue;
    }
    fprintf(stderr, "[video_player] Segment %zu: %zu frames, %dx%d\n",
            i, reader->getFrameCount(), reader->width, reader->height);
    readers[i] = std::move(reader);
  }

  // 最初の有効セグメントから解像度を取得
  int vipc_w = 0, vipc_h = 0;
  for (size_t i = 0; i < readers.size(); i++) {
    if (readers[i]) {
      vipc_w = readers[i]->width;
      vipc_h = readers[i]->height;
      break;
    }
  }

  if (vipc_w == 0 || vipc_h == 0) {
    fprintf(stderr, "[video_player] No valid segments found\n");
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

  // 準備完了。can_playerの再生開始を待機
  fprintf(stderr, "[video_player] Ready. Waiting for can_player to start...\n");
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

  // --- プリデコードスレッド関数 ---
  // 別スレッドでフレームを事前デコードしてキューに入れる
  std::thread predecode_thread([&]() {
    int next_frame_to_decode = -1;
    while (!g_exit) {
      // キューに空きがあるまで待機
      {
        std::unique_lock<std::mutex> lock(g_queue_mutex);
        g_queue_cv.wait_for(lock, std::chrono::milliseconds(10), [&] {
          return g_exit || static_cast<int>(g_decoded_queue.size()) < MAX_QUEUE_SIZE;
        });
        if (g_exit) break;
        if (static_cast<int>(g_decoded_queue.size()) >= MAX_QUEUE_SIZE) continue;
      }

      // 次にデコードすべきフレームを特定（last_frame + 1から）
      int decode_frame = next_frame_to_decode;
      if (decode_frame < 0) {
        // 最初は現在位置から開始
        std::string pos_str = params.get("CanPlaybackPosition");
        double pos = 0.0;
        if (!pos_str.empty()) {
          try { pos = std::stod(pos_str); } catch (...) {}
        }
        decode_frame = static_cast<int>(pos * FPS);
        if (decode_frame < 0) decode_frame = 0;
      } else {
        decode_frame = next_frame_to_decode + 1;
      }

      // セグメントとフレームインデックス計算
      int seg = decode_frame / (SEGMENT_SEC * FPS);
      int frame_in_seg = decode_frame % (SEGMENT_SEC * FPS);

      // 範囲チェック - 無効フレームをスキップして次に進む
      // （next_frame_to_decodeを更新しないと同じ無効フレームを永遠にリトライする）
      if (seg < 0 || seg >= static_cast<int>(segments.size()) ||
          !readers[seg] || frame_in_seg >= static_cast<int>(readers[seg]->getFrameCount())) {
        // BUG FIX: next_frame_to_decodeを更新して進捗させる
        // 無効なフレームをスキップし、次のセグメントの先頭または適切な位置に進む
        if (seg >= static_cast<int>(segments.size())) {
          // 全セグメント超過 → 最初に戻る（ループなし場合は待機）
          next_frame_to_decode = -1;  // リセットして位置から再計算
        } else if (!readers[seg] || seg + 1 >= static_cast<int>(segments.size())) {
          // 現在セグメントが無効 or 最終セグメントの末尾 → 次セグメント先頭に進む
          next_frame_to_decode = decode_frame;  // 更新して次は decode_frame + 1
        } else {
          // 次のセグメントの先頭にジャンプ
          next_frame_to_decode = (seg + 1) * SEGMENT_SEC * FPS - 1;  // -1して次に+1される
        }
        fprintf(stderr, "[video_player] Predecode: skipping out-of-range frame %d (seg=%d, frame_in_seg=%d)\n",
                decode_frame, seg, frame_in_seg);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }

      // VisionIPCバッファを取得
      VisionBuf *vipc_buf = vipc->get_buffer(VISION_STREAM_ROAD);
      if (!vipc_buf) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }

      // デコード
      FrameReader *reader = readers[seg].get();
      fprintf(stderr, "[video_player] Predecode: decoding frame %d (seg=%d, frame_in_seg=%d)\n",
              decode_frame, seg, frame_in_seg);
      bool ok = reader->get(frame_in_seg, vipc_buf);
      fprintf(stderr, "[video_player] Predecode: frame %d decode result=%d\n", decode_frame, ok);

      DecodedFrame df;
      df.frame_id = decode_frame;
      df.vipc_buf = vipc_buf;
      df.valid = ok;

      {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        g_decoded_queue.push(df);
      }
      g_queue_cv.notify_one();

      next_frame_to_decode = decode_frame;
    }
  });

  // メイン再生ループ - プリデコードバッファから送信
  int last_frame = -1;
  size_t frames_sent = 0;
  size_t frames_skipped = 0;
  double synced_position = 0.0;
  double synced_speed = 1.0;
  bool synced_paused = false;
  auto last_params_read = std::chrono::steady_clock::now();
  auto last_frame_time = std::chrono::steady_clock::now();
  static constexpr double PARAMS_READ_INTERVAL = 0.1;
  static constexpr double FRAME_INTERVAL = 1.0 / FPS;

  fprintf(stderr, "[video_player] Playback started (async pre-decode mode)\n");

  while (!g_exit) {
    if (!params.getBool("CAN_PLAYBACK")) {
      fprintf(stderr, "[video_player] CAN_PLAYBACK disabled, exiting\n");
      break;
    }

    auto now = std::chrono::steady_clock::now();
    double time_since_read = std::chrono::duration<double>(now - last_params_read).count();

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

    if (synced_paused) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }

    double current_pos = synced_position + time_since_read * synced_speed;

    // フレームタイミング制御
    double time_since_frame = std::chrono::duration<double>(now - last_frame_time).count();
    if (time_since_frame < FRAME_INTERVAL * 0.9) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }

    int total_frame = static_cast<int>(current_pos * FPS);
    if (total_frame < 0) total_frame = 0;

    if (total_frame == last_frame) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }

    // セグメント範囲チェック
    int seg = total_frame / (SEGMENT_SEC * FPS);
    if (seg >= static_cast<int>(segments.size())) {
      // キューをクリア（VisionIpcServerはリングバッファのため、
      // 未送信バッファは次のget_buffer()で上書きされる）
      {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        while (!g_decoded_queue.empty()) g_decoded_queue.pop();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      last_frame = -1;
      continue;
    }

    // キューからフレームを取得
    // NOTE: VisionIpcServerはリングバッファ方式のため、send()されなかった
    // バッファは次のget_buffer()呼び出しで上書きされる。明示的な解放は不要。
    DecodedFrame frame_to_send;
    bool found = false;
    {
      std::unique_lock<std::mutex> lock(g_queue_mutex);
      // キュー内の古いフレームをスキップ
      while (!g_decoded_queue.empty() && g_decoded_queue.front().frame_id < total_frame) {
        frames_skipped++;
        g_decoded_queue.pop();
      }
      if (!g_decoded_queue.empty() && g_decoded_queue.front().frame_id == total_frame) {
        frame_to_send = g_decoded_queue.front();
        g_decoded_queue.pop();
        found = true;
      }
    }
    g_queue_cv.notify_one();  // プリデコードスレッドに空きを通知

    if (!found) {
      // プリデコードバッファにない場合は少し待ってリトライ
      // ただし、遅延が大きい場合はフレームスキップ
      if (total_frame - last_frame > 5) {
        // 大きく遅れている - フレームスキップして追いつく
        last_frame = total_frame - 1;
        frames_skipped++;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }

    if (!frame_to_send.valid || !frame_to_send.vipc_buf) {
      // デコード失敗フレーム - バッファはリングバッファで再利用される
      fprintf(stderr, "[video_player] WARNING: frame %d invalid (valid=%d, buf=%p), skipping. sent=%zu\n",
              total_frame, frame_to_send.valid, frame_to_send.vipc_buf, frames_sent);
      last_frame = total_frame;
      continue;
    }

    VisionIpcBufExtra extra = {};
    extra.frame_id = static_cast<uint64_t>(total_frame);
    extra.timestamp_sof = static_cast<uint64_t>(current_pos * 1e9);
    extra.timestamp_eof = static_cast<uint64_t>((current_pos + FRAME_INTERVAL) * 1e9);
    vipc->send(frame_to_send.vipc_buf, &extra, false);
    frames_sent++;
    if (frames_sent <= 3) {
      fprintf(stderr, "[video_player] Sent frame #%zu: frame_id=%d, buf=%p\n",
              frames_sent, total_frame, frame_to_send.vipc_buf);
    }
    last_frame = total_frame;
    last_frame_time = std::chrono::steady_clock::now();

    // 定期的な進捗ログ（100フレームごと）
    if (frames_sent % 100 == 1) {
      fprintf(stderr, "[video_player] Progress: sent=%zu, skipped=%zu, frame=%d, pos=%.2fs\n",
              frames_sent, frames_skipped, total_frame, current_pos);
    }
  }

  // プリデコードスレッドの終了待ち
  g_queue_cv.notify_all();
  if (predecode_thread.joinable()) {
    predecode_thread.join();
  }

  fprintf(stderr, "[video_player] Exiting. Sent %zu frames, skipped %zu frames.\n",
          frames_sent, frames_skipped);

  // VisionIPCサーバーのクリーンアップ
  vipc.reset();

  return 0;
}