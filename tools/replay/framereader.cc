#include "tools/replay/framereader.h"

#include <algorithm>
#include <map>
#include <memory>
#include <tuple>
#include <utility>

#include "common/util.h"
#include "third_party/libyuv/include/libyuv.h"
#include "tools/replay/util.h"

#ifdef __APPLE__
#define HW_DEVICE_TYPE AV_HWDEVICE_TYPE_VIDEOTOOLBOX
#define HW_PIX_FMT AV_PIX_FMT_VIDEOTOOLBOX
#elif defined(QCOM2)
#define HW_DEVICE_TYPE AV_HWDEVICE_TYPE_VAAPI
#define HW_PIX_FMT AV_PIX_FMT_VAAPI
#else
#define HW_DEVICE_TYPE AV_HWDEVICE_TYPE_CUDA
#define HW_PIX_FMT AV_PIX_FMT_CUDA
#endif

namespace {

enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts) {
  enum AVPixelFormat *hw_pix_fmt = reinterpret_cast<enum AVPixelFormat *>(ctx->opaque);
  for (const enum AVPixelFormat *p = pix_fmts; *p != -1; p++) {
    if (*p == *hw_pix_fmt) return *p;
  }
  rWarning("Please run replay with the --no-hw-decoder flag!");
  *hw_pix_fmt = AV_PIX_FMT_NONE;
  return AV_PIX_FMT_YUV420P;
}

struct DecoderManager {
  VideoDecoder *acquire(CameraType type, AVCodecParameters *codecpar, bool hw_decoder) {
    auto key = std::tuple(type, codecpar->width, codecpar->height);
    std::unique_lock lock(mutex_);
    if (auto it = decoders_.find(key); it != decoders_.end()) {
      return it->second.get();
    }

    auto decoder = std::make_unique<VideoDecoder>();
    if (!decoder->open(codecpar, hw_decoder)) {
      decoder.reset(nullptr);
    }
    decoders_[key] = std::move(decoder);
    return decoders_[key].get();
  }

  std::mutex mutex_;
  std::map<std::tuple<CameraType, int, int>, std::unique_ptr<VideoDecoder>> decoders_;
};

DecoderManager decoder_manager;

}  // namespace

FrameReader::FrameReader() {
  av_log_set_level(AV_LOG_QUIET);
}

FrameReader::~FrameReader() {
  if (input_ctx) avformat_close_input(&input_ctx);
}

bool FrameReader::load(CameraType type, const std::string &url, bool no_hw_decoder, std::atomic<bool> *abort, bool local_cache, int chunk_size, int retries) {
  auto local_file_path = url.find("https://") == 0 ? cacheFilePath(url) : url;
  if (!util::file_exists(local_file_path)) {
    FileReader f(local_cache, chunk_size, retries);
    if (f.read(url, abort).empty()) {
      return false;
    }
  }
  return loadFromFile(type, local_file_path, no_hw_decoder, abort);
}

bool FrameReader::loadFromFile(CameraType type, const std::string &file, bool no_hw_decoder, std::atomic<bool> *abort) {
  if (avformat_open_input(&input_ctx, file.c_str(), nullptr, nullptr) != 0 ||
      avformat_find_stream_info(input_ctx, nullptr) < 0) {
    rError("Failed to open input file or find video stream");
    return false;
  }
  input_ctx->probesize = 10 * 1024 * 1024;  // 10MB

  decoder_ = decoder_manager.acquire(type, input_ctx->streams[0]->codecpar, !no_hw_decoder);
  if (!decoder_) {
    return false;
  }
  width = decoder_->width;
  height = decoder_->height;

  AVPacket pkt;
  packets_info.reserve(60 * 20);  // 20fps, one minute
  while (!(abort && *abort) && av_read_frame(input_ctx, &pkt) == 0) {
    packets_info.emplace_back(PacketInfo{.flags = pkt.flags, .pos = pkt.pos});
    av_packet_unref(&pkt);
  }
  avio_seek(input_ctx->pb, 0, SEEK_SET);
  return !packets_info.empty();
}

bool FrameReader::get(int idx, VisionBuf *buf) {
  if (!buf || idx < 0 || idx >= packets_info.size()) {
    return false;
  }

  std::lock_guard<std::mutex> lock(decode_mutex_);

  // Check frame cache first
  auto it = frame_cache_.find(idx);
  if (it != frame_cache_.end()) {
    const auto &cached = it->second;
    int copy_width = std::min(buf->stride, cached.stride);
    for (int row = 0; row < height; row++) {
      memcpy(buf->y + row * buf->stride, cached.y_data.data() + row * cached.stride, copy_width);
    }
    for (int row = 0; row < height / 2; row++) {
      memcpy(buf->uv + row * buf->stride, cached.uv_data.data() + row * cached.stride, copy_width);
    }
    return true;
  }

  // Decode and cache
  bool result = decoder_->decode(this, idx, buf);
  if (result && max_cache_frames_ > 0 && frame_cache_.size() < max_cache_frames_) {
    auto &cached = frame_cache_[idx];
    cached.stride = buf->stride;
    size_t y_size = static_cast<size_t>(height) * buf->stride;
    size_t uv_size = static_cast<size_t>(height / 2) * buf->stride;
    cached.y_data.assign(buf->y, buf->y + y_size);
    cached.uv_data.assign(buf->uv, buf->uv + uv_size);
  }
  return result;
}

void FrameReader::preCache(int from_idx, int count) {
  if (!decoder_ || width <= 0 || height <= 0) return;

  size_t y_size = static_cast<size_t>(width) * height;
  size_t uv_size = static_cast<size_t>(width) * height / 2;
  std::vector<uint8_t> tmp_storage(y_size + uv_size);

  VisionBuf tmp_buf;
  tmp_buf.y = tmp_storage.data();
  tmp_buf.uv = tmp_storage.data() + y_size;
  tmp_buf.stride = width;

  int end_idx = std::min(from_idx + count, static_cast<int>(packets_info.size()));
  for (int i = from_idx; i < end_idx && !cache_abort_; i++) {
    std::lock_guard<std::mutex> lock(decode_mutex_);

    if (frame_cache_.count(i)) continue;
    if (frame_cache_.size() >= max_cache_frames_) break;

    if (decoder_->decode(this, i, &tmp_buf)) {
      auto &cached = frame_cache_[i];
      cached.stride = width;
      cached.y_data.assign(tmp_buf.y, tmp_buf.y + y_size);
      cached.uv_data.assign(tmp_buf.uv, tmp_buf.uv + uv_size);
    }
  }
}

// class VideoDecoder

VideoDecoder::VideoDecoder() {
  av_frame_ = av_frame_alloc();
  hw_frame_ = av_frame_alloc();
}

VideoDecoder::~VideoDecoder() {
  if (hw_device_ctx) av_buffer_unref(&hw_device_ctx);
  if (decoder_ctx) avcodec_free_context(&decoder_ctx);
  av_frame_free(&av_frame_);
  av_frame_free(&hw_frame_);
}

bool VideoDecoder::open(AVCodecParameters *codecpar, bool hw_decoder) {
  const AVCodec *decoder = avcodec_find_decoder(codecpar->codec_id);
  if (!decoder) return false;

  decoder_ctx = avcodec_alloc_context3(decoder);
  if (!decoder_ctx || avcodec_parameters_to_context(decoder_ctx, codecpar) != 0) {
    rError("Failed to allocate or initialize codec context");
    return false;
  }
  width = (decoder_ctx->width + 3) & ~3;
  height = decoder_ctx->height;

  if (hw_decoder && !initHardwareDecoder(HW_DEVICE_TYPE)) {
    rWarning("No device with hardware decoder found. fallback to CPU decoding.");
  }

  if (avcodec_open2(decoder_ctx, decoder, nullptr) < 0) {
    rError("Failed to open codec");
    return false;
  }
  return true;
}

bool VideoDecoder::initHardwareDecoder(AVHWDeviceType hw_device_type) {
  const AVCodecHWConfig *config = nullptr;
  for (int i = 0; (config = avcodec_get_hw_config(decoder_ctx->codec, i)) != nullptr; i++) {
    if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX && config->device_type == hw_device_type) {
      hw_pix_fmt = config->pix_fmt;
      break;
    }
  }
  if (!config) {
    rWarning("Hardware configuration not found");
    return false;
  }

  int ret = av_hwdevice_ctx_create(&hw_device_ctx, hw_device_type, nullptr, nullptr, 0);
  if (ret < 0) {
    hw_pix_fmt = AV_PIX_FMT_NONE;
    rWarning("Failed to create specified HW device %d.", ret);
    return false;
  }

  decoder_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
  decoder_ctx->opaque = &hw_pix_fmt;
  decoder_ctx->get_format = get_hw_format;
  return true;
}

bool VideoDecoder::decode(FrameReader *reader, int idx, VisionBuf *buf) {
  int from_idx = idx;
  if (idx != reader->prev_idx + 1) {
    // seeking to the nearest key frame
    for (int i = idx; i >= 0; --i) {
      if (reader->packets_info[i].flags & AV_PKT_FLAG_KEY) {
        from_idx = i;
        break;
      }
    }
    avio_seek(reader->input_ctx->pb, reader->packets_info[from_idx].pos, SEEK_SET);
  }
  reader->prev_idx = idx;

  bool result = false;
  AVPacket pkt;
  for (int i = from_idx; i <= idx; ++i) {
    if (av_read_frame(reader->input_ctx, &pkt) == 0) {
      AVFrame *f = decodeFrame(&pkt);
      if (f && i == idx) {
        result = copyBuffer(f, buf);
      }
      av_packet_unref(&pkt);
    }
  }
  return result;
}

AVFrame *VideoDecoder::decodeFrame(AVPacket *pkt) {
  int ret = avcodec_send_packet(decoder_ctx, pkt);
  if (ret < 0) {
    rError("Error sending a packet for decoding: %d", ret);
    return nullptr;
  }

  ret = avcodec_receive_frame(decoder_ctx, av_frame_);
  if (ret != 0) {
    rError("avcodec_receive_frame error: %d", ret);
    return nullptr;
  }

  if (av_frame_->format == hw_pix_fmt && av_hwframe_transfer_data(hw_frame_, av_frame_, 0) < 0) {
    rError("error transferring frame data from GPU to CPU");
    return nullptr;
  }
  return (av_frame_->format == hw_pix_fmt) ? hw_frame_ : av_frame_;
}

bool VideoDecoder::copyBuffer(AVFrame *f, VisionBuf *buf) {
  if (hw_pix_fmt == HW_PIX_FMT) {
    for (int i = 0; i < height/2; i++) {
      memcpy(buf->y + (i*2 + 0)*buf->stride, f->data[0] + (i*2 + 0)*f->linesize[0], width);
      memcpy(buf->y + (i*2 + 1)*buf->stride, f->data[0] + (i*2 + 1)*f->linesize[0], width);
      memcpy(buf->uv + i*buf->stride, f->data[1] + i*f->linesize[1], width);
    }
  } else {
    libyuv::I420ToNV12(f->data[0], f->linesize[0],
                       f->data[1], f->linesize[1],
                       f->data[2], f->linesize[2],
                       buf->y, buf->stride,
                       buf->uv, buf->stride,
                       width, height);
  }
  return true;
}
