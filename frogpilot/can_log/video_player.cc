// video_player.cc - HEVC映像をVision IPCで配信するCAN Playback用バイナリ
//
// can_player.pyとParams経由で同期し、fcamera.hevcをFrameReaderで
// デコードしてVisionIpcServerでUIに配信する。
//
// リファクタリング版:
// - JSON集約読み込み: CanPlaybackState から一括で再生状態を取得
// - フォールバック: JSON取得失敗時は個別キーから読み取り
// - フレームプリフェッチ: 別スレッドで先読みデコード、リングバッファでキャッシュ
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
static const int PREFETCH_BUFFER_SIZE = 8;  // プリフェッチリングバッファサイズ
static std::atomic<bool> g_exit{false};

// シグナルハンドラ
static void on_signal(int sig) { g_exit = true; }

// --- 簡易JSON値抽出ヘルパー ---
// フラットなJSONオブジェクトからキーに対応する値を文字列として抽出する。
// ネストされたオブジェクトや配列はサポートしない。

static std::string json_get_value(const std::string &json, const std::string &key) {
  std::string search = "\"" + key + "\":";
  auto pos = json.find(search);
  if (pos == std::string::npos) return "";
  pos += search.size();
  // 空白をスキップ
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r')) {
    pos++;
  }
  if (pos >= json.size()) return "";

  if (json[pos] == '"') {
    // 文字列値: "..." を抽出
    pos++;
    auto end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
  } else {
    // 数値またはブール値: カンマまたは } までを抽出
    auto end = json.find_first_of(",} \t\n\r", pos);
    if (end == std::string::npos) end = json.size();
    return json.substr(pos, end - pos);
  }
}

// --- 再生状態構造体 ---

struct PlaybackState {
  double position = 0.0;
  double duration = 0.0;
  double speed = 1.0;
  bool playing = false;
  std::string real_time;
  int loading_progress = 0;
};

/// CanPlaybackState JSON から再生状態をパース。失敗時は個別キーにフォールバック。
static PlaybackState read_playback_state(Params &params) {
  PlaybackState state;

  // 1) JSON集約読み取りを試行
  std::string state_json = params.get("CanPlaybackState");
  if (!state_json.empty()) {
    std::string pos_str = json_get_value(state_json, "position");
    std::string dur_str = json_get_value(state_json, "duration");
    std::string spd_str = json_get_value(state_json, "speed");
    std::string play_str = json_get_value(state_json, "playing");
    std::string rt_str = json_get_value(state_json, "real_time");
    std::string lp_str = json_get_value(state_json, "loading_progress");

    if (!pos_str.empty()) {
      try { state.position = std::stod(pos_str); } catch (...) {}
      try { state.duration = std::stod(dur_str); } catch (...) {}
      try { state.speed = std::stod(spd_str); } catch (...) {}
      state.playing = (play_str == "true" || play_str == "1");
      state.real_time = rt_str;
      try { state.loading_progress = std::stoi(lp_str); } catch (...) {}
      return state;
    }
    // JSONは存在したがパース失敗 → フォールバックへ
  }

  // 2) フォールバック: 個別キーから読み取り（後方互換）
  std::string pos_str = params.get("CanPlaybackPosition");
  if (!pos_str.empty()) {
    try { state.position = std::stod(pos_str); } catch (...) {}
  }
  std::string dur_str = params.get("CanPlaybackDuration");
  if (!dur_str.empty()) {
    try { state.duration = std::stod(dur_str); } catch (...) {}
  }
  std::string spd_str = params.get("CanPlaybackSpeed");
  if (!spd_str.empty()) {
    try { state.speed = std::stod(spd_str); } catch (...) {}
  }
  std::string playing_str = params.get("CanPlaybackPlaying");
  state.playing = (playing_str == "1");
  state.real_time = params.get("CanPlaybackRealTime");
  std::string lp_str = params.get("CanPlaybackLoadingProgress");
  if (!lp_str.empty()) {
    try { state.loading_progress = std::stoi(lp_str); } catch (...) {}
  }

  return state;
}

// --- プリフェッチフレームエントリ ---

struct PrefetchedFrame {
  int total_frame = -1;          // グローバルフレームインデックス
  std::vector<uint8_t> nv12_data;  // NV12 データ (Y + UV)
  bool valid = false;
};

// --- プリフェッチバッファ管理クラス ---

class PrefetchBuffer {
public:
  PrefetchBuffer(size_t capacity, size_t /*frame_data_size*/)
    : capacity_(capacity), buffer_(capacity) {}

  /// 指定フレームがバッファに存在すれば NV12 データを取得
  bool get(int total_frame, std::vector<uint8_t> &out_data) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &entry : buffer_) {
      if (entry.valid && entry.total_frame == total_frame) {
        out_data = entry.nv12_data;
        return true;
      }
    }
    return false;
  }

  /// プリフェッチスレッドがデコード結果を格納
  void put(int total_frame, const uint8_t *data, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    // 既に存在するか確認（重複防止）
    for (auto &entry : buffer_) {
      if (entry.valid && entry.total_frame == total_frame) return;
    }
    // 最も古いエントリ（write_idx_）を上書き
    auto &entry = buffer_[write_idx_];
    entry.total_frame = total_frame;
    entry.nv12_data.assign(data, data + size);
    entry.valid = true;
    write_idx_ = (write_idx_ + 1) % capacity_;
  }

  /// バッファを全クリア（シーク時等に呼び出し）
  void clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &entry : buffer_) {
      entry.valid = false;
      entry.total_frame = -1;
      entry.nv12_data.clear();
    }
    write_idx_ = 0;
  }

  /// 指定フレームがバッファに存在するか確認
  bool has(int total_frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &entry : buffer_) {
      if (entry.valid && entry.total_frame == total_frame) return true;
    }
    return false;
  }

private:
  size_t capacity_;
  std::vector<PrefetchedFrame> buffer_;
  size_t write_idx_ = 0;
  std::mutex mutex_;
};

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

/// NV12データサイズを計算（プリフェッチバッファ用）
static size_t calc_nv12_data_size(int w, int h) {
#ifdef QCOM2
  int stride = VENUS_Y_STRIDE(COLOR_FMT_NV12, w);
  int scanlines = VENUS_Y_SCANLINES(COLOR_FMT_NV12, h);
  // NV12: Y平面(stride * scanlines) + UV平面(stride * scanlines/2)
  return static_cast<size_t>(stride) * scanlines * 3 / 2;
#else
  return static_cast<size_t>(w) * h * 3 / 2;
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

  // 全セグメントのFrameReaderを作成（no_hw_decoder=true）
  std::vector<std::unique_ptr<FrameReader>> readers(segments.size());
  for (size_t i = 0; i < segments.size(); i++) {
    std::string hevc = (fs::path(segments[i]) / "fcamera.hevc").string();
    auto reader = std::make_unique<FrameReader>();
    if (!reader->loadFromFile(RoadCam, hevc, true)) {
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

  // プリフェッチバッファの初期化
  size_t nv12_data_size = calc_nv12_data_size(vipc_w, vipc_h);
  PrefetchBuffer prefetch_buf(PREFETCH_BUFFER_SIZE, nv12_data_size);
  fprintf(stderr, "[video_player] Prefetch buffer: %d frames, %zu bytes/frame (~%.1f MB total)\n",
          PREFETCH_BUFFER_SIZE, nv12_data_size,
          static_cast<double>(PREFETCH_BUFFER_SIZE * nv12_data_size) / (1024.0 * 1024.0));

  // NV12レイアウト情報（プリフェッチ用）
  auto nv12_info = calc_nv12_info(vipc_w, vipc_h);
  size_t stride = std::get<0>(nv12_info);
  size_t scanlines = std::get<1>(nv12_info);
  size_t buf_size = std::get<2>(nv12_info);
  size_t uv_offset = stride * scanlines;

  // プリフェッチスレッド用の共有状態
  std::mutex prefetch_mutex;
  std::condition_variable prefetch_cv;
  std::atomic<int> prefetch_current_frame{-1};   // プリフェッチの基準フレーム
  std::atomic<bool> prefetch_seek{false};         // シーク検出フラグ
  std::atomic<double> prefetch_speed{1.0};        // 現在の再生速度

  // プリフェッチスレッド
  std::thread prefetch_thread([&]() {
    fprintf(stderr, "[video_player] Prefetch thread started\n");

    while (!g_exit) {
      int base_frame = prefetch_current_frame.load();
      double speed = prefetch_speed.load();

      if (base_frame < 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }

      // シーク検出時はバッファをクリア
      if (prefetch_seek.exchange(false)) {
        prefetch_buf.clear();
      }

      // プリフェッチ対象フレームの計算
      // 倍速時はスキップ間隔を考慮して必要なフレームをデコード
      int frame_step = std::max(1, static_cast<int>(speed));
      int seg_frames = SEGMENT_SEC * FPS;

      for (int i = 1; i <= PREFETCH_BUFFER_SIZE; i++) {
        if (g_exit) break;

        int target_frame = base_frame + i * frame_step;
        if (target_frame < 0) continue;

        // 既にバッファにある場合はスキップ
        if (prefetch_buf.has(target_frame)) continue;

        // セグメントとフレームインデックス
        int seg = target_frame / seg_frames;
        int frame_in_seg = target_frame % seg_frames;

        if (seg >= static_cast<int>(segments.size())) continue;
        if (seg < 0 || !readers[seg]) continue;

        FrameReader *reader = readers[seg].get();
        if (frame_in_seg >= static_cast<int>(reader->getFrameCount())) continue;

        // 一時バッファにデコード
        std::vector<uint8_t> temp_data(nv12_data_size);
        VisionBuf temp_buf = {};
        temp_buf.addr = temp_data.data();
        temp_buf.len = nv12_data_size;
        temp_buf.y = temp_data.data();
        temp_buf.uv = temp_data.data() + uv_offset;
        temp_buf.stride = stride;
        temp_buf.uv_offset = uv_offset;
        temp_buf.width = static_cast<size_t>(vipc_w);
        temp_buf.height = static_cast<size_t>(vipc_h);

        bool ok = reader->get(frame_in_seg, &temp_buf);
        if (ok) {
          prefetch_buf.put(target_frame, temp_data.data(), nv12_data_size);
        }
      }

      // 次のプリフェッチサイクルまで待機
      {
        std::unique_lock<std::mutex> lock(prefetch_mutex);
        prefetch_cv.wait_for(lock, std::chrono::milliseconds(50), [&]() {
          return g_exit.load() || prefetch_seek.load();
        });
      }
    }

    fprintf(stderr, "[video_player] Prefetch thread exiting\n");
  });

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
    PlaybackState init_state = read_playback_state(params);
    if (init_state.playing) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (g_exit) {
    prefetch_thread.join();
    return 0;
  }

  // メイン再生ループ - プリフェッチバッファ + 同期デコードフォールバック
  int last_frame = -1;
  size_t frames_sent = 0;
  size_t frames_from_prefetch = 0;
  size_t frames_from_sync = 0;
  double synced_position = 0.0;
  double synced_speed = 1.0;
  bool synced_paused = false;
  auto last_params_read = std::chrono::steady_clock::now();
  auto last_frame_time = std::chrono::steady_clock::now();
  static constexpr double PARAMS_READ_INTERVAL = 0.1;  // Params読み取り間隔（秒）
  static constexpr double FRAME_INTERVAL = 1.0 / FPS;  // フレーム間隔（秒）

  fprintf(stderr, "[video_player] Playback started (prefetch + sync fallback mode)\n");

  while (!g_exit) {
    // CAN_PLAYBACKが有効か確認
    if (!params.getBool("CAN_PLAYBACK")) {
      fprintf(stderr, "[video_player] CAN_PLAYBACK disabled, exiting\n");
      break;
    }

    auto now = std::chrono::steady_clock::now();
    double time_since_read = std::chrono::duration<double>(now - last_params_read).count();

    // Paramsから定期的に状態を読み取り（JSON集約 + フォールバック）
    if (time_since_read >= PARAMS_READ_INTERVAL) {
      PlaybackState state = read_playback_state(params);
      synced_position = state.position;
      synced_speed = state.speed;
      synced_paused = !state.playing;

      // 速度変更をプリフェッチスレッドに通知
      double old_speed = prefetch_speed.exchange(synced_speed);
      if (old_speed != synced_speed) {
        prefetch_speed.store(synced_speed);
      }

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

    // シーク検出: フレームが大幅に飛んだ場合
    if (last_frame >= 0 && std::abs(total_frame - last_frame) > FPS) {
      prefetch_seek.store(true);
      prefetch_buf.clear();
      prefetch_cv.notify_one();
      fprintf(stderr, "[video_player] Seek detected: %d -> %d, cleared prefetch buffer\n", last_frame, total_frame);
    }

    // プリフェッチスレッドに現在フレームを通知
    prefetch_current_frame.store(total_frame);
    prefetch_cv.notify_one();

    // セグメントとセグメント内フレームインデックス
    int seg = total_frame / (SEGMENT_SEC * FPS);
    int frame_in_seg = total_frame % (SEGMENT_SEC * FPS);

    // セグメント範囲チェック
    if (seg >= static_cast<int>(segments.size())) {
      // ループ時: can_playerが位置をリセットするのを待つ
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      last_frame = -1;
      continue;
    }

    // セグメント範囲チェック（無効セグメント）
    if (seg < 0 || !readers[seg]) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      last_frame = total_frame;  // スキップして次へ
      continue;
    }

    // フレーム数チェック
    FrameReader *reader = readers[seg].get();
    if (frame_in_seg >= static_cast<int>(reader->getFrameCount())) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      last_frame = total_frame;  // スキップして次へ
      continue;
    }

    // VisionIPCバッファを取得
    VisionBuf *vipc_buf = vipc->get_buffer(VISION_STREAM_ROAD);
    if (!vipc_buf) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }

    // フレームデータ取得: プリフェッチバッファ → 同期デコードフォールバック
    bool frame_ok = false;

    // 1) プリフェッチバッファからの取得を試行
    std::vector<uint8_t> cached_data;
    if (prefetch_buf.get(total_frame, cached_data)) {
      // バッファにデータがある → VisionIPCバッファにコピー
      size_t copy_size = std::min(cached_data.size(), vipc_buf->len);
      memcpy(vipc_buf->addr, cached_data.data(), copy_size);
      frame_ok = true;
      frames_from_prefetch++;
    }

    // 2) フォールバック: 同期デコード
    if (!frame_ok) {
      frame_ok = reader->get(frame_in_seg, vipc_buf);
      if (frame_ok) {
        frames_from_sync++;
      }
    }

    if (!frame_ok) {
      // デコード失敗時はスキップ
      last_frame = total_frame;
      continue;
    }

    VisionIpcBufExtra extra = {};
    extra.frame_id = static_cast<uint64_t>(total_frame);
    extra.timestamp_sof = static_cast<uint64_t>(current_pos * 1e9);
    extra.timestamp_eof = static_cast<uint64_t>((current_pos + FRAME_INTERVAL) * 1e9);
    vipc->send(vipc_buf, &extra, false);
    frames_sent++;
    last_frame = total_frame;
    last_frame_time = std::chrono::steady_clock::now();
  }

  fprintf(stderr, "[video_player] Exiting. Sent %zu frames total (prefetch: %zu, sync: %zu).\n",
          frames_sent, frames_from_prefetch, frames_from_sync);

  // プリフェッチスレッドの停止
  prefetch_cv.notify_one();
  prefetch_thread.join();

  // VisionIPCサーバーのクリーンアップ
  vipc.reset();

  return 0;
}