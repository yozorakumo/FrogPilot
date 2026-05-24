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
  return decoder_->decode(this, idx, buf);
}

// class VideoDecoder

VideoDecoder::VideoDecoder() {
  av_frame_ = av_frame_alloc();
  hw_frame_ = av_frame_alloc();
}

VideoDecoder::~VideoDecoder() {
  if (hw_device_ctx) av_buffer_unref(&hw_device_ctx);
  if (decoder_ctx) avcodec_free_context(&decoder_ctx);
  if (stored_codecpar_) avcodec_parameters_free(&stored_codecpar_);
  av_frame_free(&av_frame_);
  av_frame_free(&hw_frame_);
}

bool VideoDecoder::open(AVCodecParameters *codecpar, bool hw_decoder) {
  const AVCodec *decoder = nullptr;

#ifdef QCOM2
  // On QCOM2 (Snapdragon Venus), try direct V4L2 ION decoder for HEVC.
  // This uses the same ION USERPTR pattern as V4LEncoder, bypassing FFmpeg's
  // broken V4L2 M2M wrapper that doesn't handle Qualcomm ION buffers correctly.
  if (hw_decoder && codecpar->codec_id == AV_CODEC_ID_HEVC) {
    int w = (codecpar->width + 3) & ~3;
    int h = codecpar->height;
    fprintf(stderr, "[VideoDecoder] Trying direct V4L2 ION decoder (%dx%d)...\n", w, h);
    if (v4l_decoder_.open(w, h)) {
      use_v4l_direct_ = true;
      // Store codecpar for potential CPU fallback if V4L2 decode fails at runtime
      stored_codecpar_ = avcodec_parameters_alloc();
      if (stored_codecpar_) {
        avcodec_parameters_copy(stored_codecpar_, codecpar);
      }
      width = w;
      height = h;
      fprintf(stderr, "[VideoDecoder] Direct V4L2 ION decoder active (%dx%d)\n", width, height);
      return true;
    }
    fprintf(stderr, "[VideoDecoder] Direct V4L2 ION decoder failed, falling back to FFmpeg\n");
  }
#endif

  if (!decoder) {
    decoder = avcodec_find_decoder(codecpar->codec_id);
  }
  if (!decoder) return false;

  decoder_ctx = avcodec_alloc_context3(decoder);
  if (!decoder_ctx || avcodec_parameters_to_context(decoder_ctx, codecpar) != 0) {
    rError("Failed to allocate or initialize codec context");
    return false;
  }
  width = (decoder_ctx->width + 3) & ~3;
  height = decoder_ctx->height;

  bool is_v4l2m2m = (strcmp(decoder->name, "hevc_v4l2m2m") == 0);

  // Only try CUDA/VideoToolbox HW accel for non-V4L2M2M decoders.
  // V4L2 M2M handles hardware access internally.
  if (hw_decoder && !is_v4l2m2m && !initHardwareDecoder(HW_DEVICE_TYPE)) {
    rWarning("No device with hardware decoder found. fallback to CPU decoding.");
  }

  // Enable multi-threaded software decoding for better performance on multi-core CPUs
  if (!is_v4l2m2m && hw_pix_fmt == AV_PIX_FMT_NONE) {
    int cpu_cores = std::max(1, (int)sysconf(_SC_NPROCESSORS_ONLN));
    decoder_ctx->thread_count = std::min(cpu_cores, 4);
    decoder_ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    fprintf(stderr, "[VideoDecoder] CPU decoder: %d threads, %dx%d\n",
            decoder_ctx->thread_count, width, height);
  }

  if (avcodec_open2(decoder_ctx, decoder, nullptr) < 0) {
    // If V4L2 M2M failed to open, fall back to standard CPU decoder
    if (is_v4l2m2m) {
      rWarning("V4L2 M2M failed to open, falling back to CPU decoder");
      avcodec_free_context(&decoder_ctx);
      is_v4l2m2m = false;
      decoder = avcodec_find_decoder(codecpar->codec_id);
      if (!decoder) return false;
      decoder_ctx = avcodec_alloc_context3(decoder);
      if (!decoder_ctx || avcodec_parameters_to_context(decoder_ctx, codecpar) != 0) return false;
      width = (decoder_ctx->width + 3) & ~3;
      height = decoder_ctx->height;

      // Enable multi-threaded CPU decoding for fallback path too
      int cpu_cores = std::max(1, (int)sysconf(_SC_NPROCESSORS_ONLN));
      decoder_ctx->thread_count = std::min(cpu_cores, 4);
      decoder_ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
      fprintf(stderr, "[VideoDecoder] CPU fallback decoder: %d threads, %dx%d\n",
              decoder_ctx->thread_count, width, height);

      if (avcodec_open2(decoder_ctx, decoder, nullptr) < 0) return false;
    } else {
      rError("Failed to open codec");
      return false;
    }
  }

  v4l2m2m_ = is_v4l2m2m;
  if (v4l2m2m_) {
    fprintf(stderr, "[VideoDecoder] V4L2 M2M hardware decoder active (%dx%d)\n", width, height);
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
#ifdef QCOM2
  if (use_v4l_direct_ && !v4l_fallback_to_cpu_) {
    bool result = decodeV4L(reader, idx, buf);
    if (!result && !v4l_fallback_to_cpu_) {
      fprintf(stderr, "[VideoDecoder] V4L2 decode failed, falling back to CPU decoder\n");
      if (initCPUFallback()) {
        v4l_fallback_to_cpu_ = true;
        use_v4l_direct_ = false;
        // Force seek on next decode (V4L2 consumed packets, file position is wrong)
        // prev_idx = -2 ensures idx != prev_idx + 1 for any idx >= 0
        reader->prev_idx = -2;
        result = decode(reader, idx, buf);
      }
    }
    return result;
  }
#endif

  int from_idx = idx;
  if (idx != reader->prev_idx + 1) {
    // Flush decoder on seek to avoid stale buffered frames (critical for V4L2 M2M)
    avcodec_flush_buffers(decoder_ctx);

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
  if (ret < 0 && ret != AVERROR(EAGAIN)) {
    rError("Error sending a packet for decoding: %d", ret);
    return nullptr;
  }

  ret = avcodec_receive_frame(decoder_ctx, av_frame_);
  if (ret == AVERROR(EAGAIN)) {
    // Decoder needs more packets before outputting a frame (normal for threaded decoding)
    return nullptr;
  }
  if (ret < 0) {
    rError("avcodec_receive_frame error: %d", ret);
    return nullptr;
  }

  if (av_frame_->format == hw_pix_fmt && av_hwframe_transfer_data(hw_frame_, av_frame_, 0) < 0) {
    rError("error transferring frame data from GPU to CPU");
    return nullptr;
  }
  return (av_frame_->format == hw_pix_fmt) ? hw_frame_ : av_frame_;
}

bool VideoDecoder::decodeV4L(FrameReader *reader, int idx, VisionBuf *buf) {
#ifdef QCOM2
  int from_idx = idx;

  // Flush V4L decoder on seek
  if (idx != reader->prev_idx + 1) {
    v4l_decoder_.flush();

    // Find nearest key frame
    for (int i = idx; i >= 0; --i) {
      if (reader->packets_info[i].flags & AV_PKT_FLAG_KEY) {
        from_idx = i;
        break;
      }
    }
    avio_seek(reader->input_ctx->pb, reader->packets_info[from_idx].pos, SEEK_SET);
  }
  reader->prev_idx = idx;

  // Feed compressed packets to V4L decoder
  AVPacket pkt;
  for (int i = from_idx; i <= idx; ++i) {
    if (av_read_frame(reader->input_ctx, &pkt) == 0) {
      if (pkt.size > 0) {
        if (!v4l_decoder_.feed(pkt.data, pkt.size)) {
          av_packet_unref(&pkt);
          return false;
        }
      }
      av_packet_unref(&pkt);
    }
    // Drain intermediate CAPTURE buffers to prevent buffer starvation.
    // Non-blocking: re-queues decoded frames we don't need (only intermediate).
    // The target frame (i == idx) is NOT drained - getFrame() retrieves it.
    if (i < idx) {
      v4l_decoder_.drainCapture();
    }
  }

  // Get decoded NV12 frame for the target (blocking with 500ms timeout)
  return v4l_decoder_.getFrame(buf);
#else
  return false;
#endif
}

bool VideoDecoder::initCPUFallback() {
#ifdef QCOM2
  if (!stored_codecpar_) {
    fprintf(stderr, "[VideoDecoder] No stored codecpar for CPU fallback\n");
    return false;
  }

  // Close V4L decoder
  v4l_decoder_.close();
  use_v4l_direct_ = false;

  const AVCodec *decoder = avcodec_find_decoder(stored_codecpar_->codec_id);
  if (!decoder) {
    fprintf(stderr, "[VideoDecoder] CPU fallback: codec not found\n");
    return false;
  }

  decoder_ctx = avcodec_alloc_context3(decoder);
  if (!decoder_ctx || avcodec_parameters_to_context(decoder_ctx, stored_codecpar_) != 0) {
    fprintf(stderr, "[VideoDecoder] CPU fallback: failed to allocate codec context\n");
    return false;
  }

  width = (decoder_ctx->width + 3) & ~3;
  height = decoder_ctx->height;

  // Multi-threaded CPU decoding - use SLICE only to avoid frame reordering delay
  // FF_THREAD_FRAME causes output delay (thread_count-1 frames), which breaks
  // sequential decode-and-copy pattern used in video_player
  int cpu_cores = std::max(1, (int)sysconf(_SC_NPROCESSORS_ONLN));
  decoder_ctx->thread_count = std::min(cpu_cores, 4);
  decoder_ctx->thread_type = FF_THREAD_SLICE;

  if (avcodec_open2(decoder_ctx, decoder, nullptr) < 0) {
    fprintf(stderr, "[VideoDecoder] CPU fallback: failed to open codec\n");
    return false;
  }

  fprintf(stderr, "[VideoDecoder] CPU fallback active: %d threads, %dx%d\n",
          decoder_ctx->thread_count, width, height);
  return true;
#else
  return false;
#endif
}

bool VideoDecoder::copyBuffer(AVFrame *f, VisionBuf *buf) {
  if (hw_pix_fmt == HW_PIX_FMT) {
    // CUDA/VideoToolbox path - direct NV12 copy
    for (int i = 0; i < height/2; i++) {
      memcpy(buf->y + (i*2 + 0)*buf->stride, f->data[0] + (i*2 + 0)*f->linesize[0], width);
      memcpy(buf->y + (i*2 + 1)*buf->stride, f->data[0] + (i*2 + 1)*f->linesize[0], width);
      memcpy(buf->uv + i*buf->stride, f->data[1] + i*f->linesize[1], width);
    }
  } else if (f->format == AV_PIX_FMT_NV12) {
    // V4L2 M2M path - NV12 to NV12 with stride adjustment
    for (int i = 0; i < height; i++) {
      memcpy(buf->y + i * buf->stride, f->data[0] + i * f->linesize[0], width);
    }
    for (int i = 0; i < height / 2; i++) {
      memcpy(buf->uv + i * buf->stride, f->data[1] + i * f->linesize[1], width);
    }
  } else {
    // Software decode path - I420 to NV12 conversion
    libyuv::I420ToNV12(f->data[0], f->linesize[0],
                       f->data[1], f->linesize[1],
                       f->data[2], f->linesize[2],
                       buf->y, buf->stride,
                       buf->uv, buf->stride,
                       width, height);
  }
  return true;
}
