#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "msgq/visionipc/visionbuf.h"
#include "system/camerad/cameras/camera_common.h"
#include "tools/replay/filereader.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

class VideoDecoder;

class FrameReader {
public:
  FrameReader();
  ~FrameReader();
  bool load(CameraType type, const std::string &url, bool no_hw_decoder = false, std::atomic<bool> *abort = nullptr, bool local_cache = false,
            int chunk_size = -1, int retries = 0);
  bool loadFromFile(CameraType type, const std::string &file, bool no_hw_decoder = false, std::atomic<bool> *abort = nullptr);
  bool get(int idx, VisionBuf *buf);
  size_t getFrameCount() const { return packets_info.size(); }

  int width = 0, height = 0;

  VideoDecoder *decoder_ = nullptr;
  AVFormatContext *input_ctx = nullptr;
  int prev_idx = -1;
  struct PacketInfo {
    int flags;
    int64_t pos;
  };
  std::vector<PacketInfo> packets_info;

  // Frame cache for smoother playback
  struct CachedFrame {
    std::vector<uint8_t> y_data;
    std::vector<uint8_t> uv_data;
    int stride;
  };
  void setCacheSize(size_t max_frames) { max_cache_frames_ = max_frames; }
  void preCache(int from_idx, int count);
  void clearCache() { frame_cache_.clear(); }

  std::mutex decode_mutex_;
  std::unordered_map<int, CachedFrame> frame_cache_;
  size_t max_cache_frames_ = 0;
  std::atomic<bool> cache_abort_{false};
};


class VideoDecoder {
public:
  VideoDecoder();
  ~VideoDecoder();
  bool open(AVCodecParameters *codecpar, bool hw_decoder);
  bool decode(FrameReader *reader, int idx, VisionBuf *buf);
  int width = 0, height = 0;

private:
  bool initHardwareDecoder(AVHWDeviceType hw_device_type);
  AVFrame *decodeFrame(AVPacket *pkt);
  bool copyBuffer(AVFrame *f, VisionBuf *buf);

  AVFrame *av_frame_, *hw_frame_;
  AVCodecContext *decoder_ctx = nullptr;
  AVPixelFormat hw_pix_fmt = AV_PIX_FMT_NONE;
  AVBufferRef *hw_device_ctx = nullptr;
};
