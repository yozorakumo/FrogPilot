// v4l_decoder.cc - V4L2 hardware decoder for Qualcomm Venus (msm_vidc_vdec)
//
// USERPTR-based CAPTURE for non-DMABUF devices:
//   - OUTPUT: V4L2_MEMORY_USERPTR (compressed HEVC input)
//   - CAPTURE: V4L2_MEMORY_USERPTR (decoded NV12 output via ION mmap addr)
//
// Device: /dev/video32
// Driver: msm_vidc_driver, Card: msm_vidc_vdec

#ifdef QCOM2

#include <cassert>
#include <cstring>
#include <string>
#include <sys/ioctl.h>
#include <poll.h>

#include "tools/replay/v4l_decoder.h"
#include "common/util.h"
#include "common/queue.h"

#include "third_party/linux/include/msm_media_info.h"

// has to be in this order
#include "third_party/linux/include/v4l2-controls.h"
#include <linux/videodev2.h>

#define V4L2_QCOM_BUF_FLAG_CODECCONFIG 0x00020000
#define V4L2_QCOM_BUF_FLAG_EOS 0x02000000

static int env_debug_decoder = (getenv("DEBUG_V4L_DECODER") != NULL) ? atoi(getenv("DEBUG_V4L_DECODER")) : 0;

#define LOG_DEBUG(fmt, ...) do { if (env_debug_decoder) fprintf(stderr, "[V4LDecoder] " fmt "\n", ##__VA_ARGS__); } while(0)

static bool checked_ioctl(int fd, unsigned long request, void *argp) {
  int ret = util::safe_ioctl(fd, request, argp);
  if (ret != 0) {
    fprintf(stderr, "[V4LDecoder] ioctl failed: fd=%d request=0x%lx errno=%d (%s)\n",
            fd, request, errno, strerror(errno));
    return false;
  }
  return true;
}

static int try_ioctl(int fd, unsigned long request, void *argp) {
  int ret = util::safe_ioctl(fd, request, argp);
  if (ret != 0) {
    if (errno == EAGAIN) return 1;
    return -1;
  }
  return 0;
}

// Queue OUTPUT buffer (USERPTR)
static void queue_output_buffer(int fd, int index, VisionBuf *buf, uint32_t bytesused, uint32_t flags) {
  v4l2_plane plane = {
    .length = (unsigned int)buf->len,
    .m = { .userptr = (unsigned long)buf->addr, },
    .bytesused = bytesused,
    .reserved = {(unsigned int)buf->fd}
  };

  v4l2_buffer v4l_buf = {
    .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
    .index = (unsigned int)index,
    .memory = V4L2_MEMORY_USERPTR,
    .m = { .planes = &plane, },
    .length = 1,
    .flags = flags | V4L2_BUF_FLAG_TIMESTAMP_COPY,
  };

  checked_ioctl(fd, VIDIOC_QBUF, &v4l_buf);
}

// Queue CAPTURE buffer (USERPTR)
static void queue_capture_buffer(int fd, int index, VisionBuf *buf) {
  v4l2_plane plane = {
    .length = (unsigned int)buf->len,
    .m = { .userptr = (unsigned long)buf->addr, },
    .bytesused = 0,
  };

  v4l2_buffer v4l_buf = {
    .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
    .index = (unsigned int)index,
    .memory = V4L2_MEMORY_USERPTR,
    .m = { .planes = &plane, },
    .length = 1,
  };

  checked_ioctl(fd, VIDIOC_QBUF, &v4l_buf);
}

static bool request_buffers(int fd, v4l2_buf_type buf_type, unsigned int count, v4l2_memory memory) {
  struct v4l2_requestbuffers reqbuf = {
    .type = buf_type,
    .memory = memory,
    .count = count
  };
  return checked_ioctl(fd, VIDIOC_REQBUFS, &reqbuf);
}

bool V4LDecoder::open(int in_width, int in_height) {
  width = in_width;
  height = in_height;

  struct v4l2_capability cap = {};
  struct v4l2_format fmt = {};
  v4l2_buf_type buf_type = (v4l2_buf_type)0;

  // Use /dev/video32 directly for SDM845 Venus decoder
  fd = ::open("/dev/video32", O_RDWR | O_NONBLOCK);
  if (fd < 0) {
    fprintf(stderr, "[V4LDecoder] Failed to open decoder device /dev/video32: %s\n", strerror(errno));
    return false;
  }

  if (!checked_ioctl(fd, VIDIOC_QUERYCAP, &cap)) goto fail;
  if (strcmp((const char *)cap.driver, "msm_vidc_driver") != 0 ||
      strcmp((const char *)cap.card, "msm_vidc_vdec") != 0) {
    fprintf(stderr, "[V4LDecoder] Wrong device: driver=%s card=%s\n", cap.driver, cap.card);
    goto fail;
  }
  fprintf(stderr, "[V4LDecoder] Opened decoder: %s %s fd=%d\n", cap.driver, cap.card, fd);

  // Subscribe to SOURCE_CHANGE event
  {
    struct v4l2_event_subscription sub = {};
    sub.type = V4L2_EVENT_SOURCE_CHANGE;
    if (!checked_ioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub)) goto fail;
    LOG_DEBUG("Subscribed to V4L2_EVENT_SOURCE_CHANGE");
  }

  // Set OUTPUT format (compressed HEVC input)
  fmt = {};
  fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  fmt.fmt.pix_mp.width = (unsigned int)width;
  fmt.fmt.pix_mp.height = (unsigned int)height;
  fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_HEVC;
  fmt.fmt.pix_mp.field = V4L2_FIELD_ANY;
  fmt.fmt.pix_mp.colorspace = V4L2_COLORSPACE_DEFAULT;
  fmt.fmt.pix_mp.num_planes = 1;
  fmt.fmt.pix_mp.plane_fmt[0].sizeimage = 2 * 1024 * 1024;

  if (!checked_ioctl(fd, VIDIOC_S_FMT, &fmt)) goto fail;
  input_buf_size = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
  fprintf(stderr, "[V4LDecoder] OUTPUT format: HEVC %dx%d, sizeimage=%zu\n",
          fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height, input_buf_size);

  // Calculate expected CAPTURE buffer sizes
  decoded_stride = VENUS_Y_STRIDE(COLOR_FMT_NV12, width);
  output_buf_size = (size_t)VENUS_BUFFER_SIZE(COLOR_FMT_NV12, width, height);

  // Allocate ION buffers for OUTPUT (USERPTR)
  for (int i = 0; i < V4L_DEC_BUF_IN_COUNT; i++) {
    buf_in[i].allocate(input_buf_size);
    free_input_bufs.push(i);
  }

  // Request and start OUTPUT streaming (USERPTR)
  if (!request_buffers(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L_DEC_BUF_IN_COUNT, V4L2_MEMORY_USERPTR)) goto fail;
  LOG_DEBUG("OUTPUT REQBUFS: %d buffers (USERPTR)", V4L_DEC_BUF_IN_COUNT);

  buf_type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  if (!checked_ioctl(fd, VIDIOC_STREAMON, &buf_type)) goto fail;
  LOG_DEBUG("OUTPUT STREAMON successful");

  is_open = true;
  capture_ready = false;
  extradata_sent = false;
  fprintf(stderr, "[V4LDecoder] Phase 1 complete. CAPTURE will be set up after SOURCE_CHANGE.\n");
  return true;

fail:
  for (int i = 0; i < V4L_DEC_BUF_IN_COUNT; i++) {
    buf_in[i].free();
  }
  while (!free_input_bufs.empty()) free_input_bufs.pop();
  ::close(fd);
  fd = -1;
  fprintf(stderr, "[V4LDecoder] open failed, falling back to CPU decoder\n");
  return false;
}

bool V4LDecoder::setupCapture() {
  struct v4l2_format fmt = {};
  v4l2_buf_type buf_type = (v4l2_buf_type)0;

  fprintf(stderr, "[V4LDecoder] Setting up CAPTURE...\n");

  // Get actual CAPTURE format after SOURCE_CHANGE
  fmt = {};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  if (!checked_ioctl(fd, VIDIOC_G_FMT, &fmt)) {
    fprintf(stderr, "[V4LDecoder] G_FMT on CAPTURE failed\n");
    return false;
  }
  fprintf(stderr, "[V4LDecoder] CAPTURE G_FMT: pixelformat=%c%c%c%c %dx%d sizeimage=%d\n",
          fmt.fmt.pix_mp.pixelformat & 0xFF,
          (fmt.fmt.pix_mp.pixelformat >> 8) & 0xFF,
          (fmt.fmt.pix_mp.pixelformat >> 16) & 0xFF,
          (fmt.fmt.pix_mp.pixelformat >> 24) & 0xFF,
          fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
          fmt.fmt.pix_mp.plane_fmt[0].sizeimage);

  // Update stride/buf_size based on actual format
  if (fmt.fmt.pix_mp.pixelformat == V4L2_PIX_FMT_NV12) {
    decoded_stride = VENUS_Y_STRIDE(COLOR_FMT_NV12, (int)fmt.fmt.pix_mp.width);
    output_buf_size = (size_t)VENUS_BUFFER_SIZE(COLOR_FMT_NV12,
                                                  (int)fmt.fmt.pix_mp.width,
                                                  (int)fmt.fmt.pix_mp.height);
  }

  // Allocate CAPTURE ION buffers (USERPTR mode)
  for (int i = 0; i < V4L_DEC_BUF_OUT_COUNT; i++) {
    buf_out[i].allocate(output_buf_size);
    LOG_DEBUG("CAPTURE buffer %d: addr=%p len=%zu", i, buf_out[i].addr, buf_out[i].len);
  }

  // Request CAPTURE buffers with USERPTR memory (no DMA BUF required)
  if (!request_buffers(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L_DEC_BUF_OUT_COUNT, V4L2_MEMORY_USERPTR)) {
    fprintf(stderr, "[V4LDecoder] CAPTURE REQBUFS (USERPTR) failed\n");
    for (int i = 0; i < V4L_DEC_BUF_OUT_COUNT; i++) buf_out[i].free();
    return false;
  }
  fprintf(stderr, "[V4LDecoder] CAPTURE REQBUFS: %d buffers (USERPTR)\n", V4L_DEC_BUF_OUT_COUNT);

  // Start CAPTURE streaming
  buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  if (!checked_ioctl(fd, VIDIOC_STREAMON, &buf_type)) {
    fprintf(stderr, "[V4LDecoder] CAPTURE STREAMON failed\n");
    for (int i = 0; i < V4L_DEC_BUF_OUT_COUNT; i++) buf_out[i].free();
    return false;
  }
  fprintf(stderr, "[V4LDecoder] CAPTURE STREAMON successful\n");

  // Queue all CAPTURE buffers (USERPTR)
  for (int i = 0; i < V4L_DEC_BUF_OUT_COUNT; i++) {
    queue_capture_buffer(fd, i, &buf_out[i]);
  }
  fprintf(stderr, "[V4LDecoder] Queued %d CAPTURE buffers (USERPTR)\n", V4L_DEC_BUF_OUT_COUNT);

  capture_ready = true;
  fprintf(stderr, "[V4LDecoder] CAPTURE ready: stride=%d, buf_size=%zu\n",
          decoded_stride, output_buf_size);
  return true;
}

void V4LDecoder::drainOutput() {
  if (!is_open) return;
  while (true) {
    v4l2_plane plane = {};
    v4l2_buffer v4l_buf = {
      .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
      .memory = V4L2_MEMORY_USERPTR,
      .m = { .planes = &plane, },
      .length = 1,
    };
    if (try_ioctl(fd, VIDIOC_DQBUF, &v4l_buf) != 0) break;
    free_input_bufs.push(v4l_buf.index);
  }
}

void V4LDecoder::queueCaptureBuffer(int index) {
  queue_capture_buffer(fd, index, &buf_out[index]);
}

void V4LDecoder::queueOutputBuffer(int index, uint32_t bytesused, uint32_t flags) {
  queue_output_buffer(fd, index, &buf_in[index], bytesused, flags);
}

void V4LDecoder::drainCapture() {
  if (!capture_ready) return;

  struct pollfd pfd = {.fd = fd, .events = POLLIN | POLLOUT | POLLPRI, .revents = 0};
  while (true) {
    int rc = poll(&pfd, 1, 0);
    if (rc <= 0) break;

    if (pfd.revents & POLLPRI) {
      struct v4l2_event ev = {};
      util::safe_ioctl(fd, VIDIOC_DQEVENT, &ev);
    }
    if (pfd.revents & POLLIN) {
      v4l2_plane plane = {};
      v4l2_buffer v4l_buf = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
        .memory = V4L2_MEMORY_USERPTR,
        .m = { .planes = &plane, },
        .length = 1,
      };
      if (try_ioctl(fd, VIDIOC_DQBUF, &v4l_buf) != 0) break;
      queueCaptureBuffer(v4l_buf.index);
    }
    if (pfd.revents & POLLOUT) {
      v4l2_plane plane = {};
      v4l2_buffer v4l_buf = {
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
        .memory = V4L2_MEMORY_USERPTR,
        .m = { .planes = &plane, },
        .length = 1,
      };
      if (try_ioctl(fd, VIDIOC_DQBUF, &v4l_buf) != 0) break;
      free_input_bufs.push(v4l_buf.index);
    }
  }
}

bool V4LDecoder::feedExtradata(const uint8_t *data, size_t size) {
  if (!is_open) return false;
  if (extradata_sent) return true;

  extradata.assign(data, data + size);
  LOG_DEBUG("Stored extradata: %zu bytes", size);
  return true;
}

bool V4LDecoder::feed(const uint8_t *data, size_t size) {
  if (!is_open) {
    fprintf(stderr, "[V4LDecoder] feed: not open!\n");
    return false;
  }

  static int feed_call_count = 0;
  feed_call_count++;
  LOG_DEBUG("feed #%d: size=%zu, capture_ready=%d", feed_call_count, size, capture_ready);

  drainOutput();

  // Check for SOURCE_CHANGE event (non-blocking)
  if (!capture_ready) {
    struct pollfd pfd = {.fd = fd, .events = POLLPRI | POLLOUT, .revents = 0};
    int rc = poll(&pfd, 1, 0);
    LOG_DEBUG("feed #%d: SOURCE_CHANGE poll rc=%d revents=0x%x", feed_call_count, rc, pfd.revents);
    if (rc > 0 && (pfd.revents & POLLPRI)) {
      struct v4l2_event ev = {};
      if (util::safe_ioctl(fd, VIDIOC_DQEVENT, &ev) == 0 && ev.type == V4L2_EVENT_SOURCE_CHANGE) {
        struct v4l2_event_src_change *sc = (struct v4l2_event_src_change *)ev.u.data;
        fprintf(stderr, "[V4LDecoder] SOURCE_CHANGE received! changes=0x%x\n", sc->changes);
        if (!setupCapture()) {
          fprintf(stderr, "[V4LDecoder] CAPTURE setup failed\n");
          return false;
        }
      } else {
        LOG_DEBUG("feed #%d: POLLPRI but event type=%d (not SOURCE_CHANGE=%d)",
                feed_call_count, ev.type, V4L2_EVENT_SOURCE_CHANGE);
      }
    }
  }

  // Also drain OUTPUT after poll
  if (!capture_ready) {
    LOG_DEBUG("feed #%d: capture not ready, draining output", feed_call_count);
    drainOutput();
  }

  // Get a free input buffer
  int buf_idx = -1;
  if (free_input_bufs.empty()) {
    struct pollfd pfd = {.fd = fd, .events = POLLOUT | POLLPRI, .revents = 0};
    int rc = poll(&pfd, 1, 200);
    if (rc <= 0) {
      fprintf(stderr, "[V4LDecoder] feed: no OUTPUT buffers available\n");
      return false;
    }
    if (pfd.revents & POLLPRI) {
      struct v4l2_event ev = {};
      if (util::safe_ioctl(fd, VIDIOC_DQEVENT, &ev) == 0 && ev.type == V4L2_EVENT_SOURCE_CHANGE && !capture_ready) {
        struct v4l2_event_src_change *sc = (struct v4l2_event_src_change *)ev.u.data;
        fprintf(stderr, "[V4LDecoder] SOURCE_CHANGE received during feed wait! changes=0x%x\n", sc->changes);
        if (!setupCapture()) return false;
      }
    }
    v4l2_plane plane = {};
    v4l2_buffer v4l_buf = {
      .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
      .memory = V4L2_MEMORY_USERPTR,
      .m = { .planes = &plane, },
      .length = 1,
    };
    if (try_ioctl(fd, VIDIOC_DQBUF, &v4l_buf) == 0) {
      free_input_bufs.push(v4l_buf.index);
    }
  }

  if (!free_input_bufs.empty()) {
    buf_idx = free_input_bufs.pop();
  }
  if (buf_idx < 0) {
    fprintf(stderr, "[V4LDecoder] feed: failed to get OUTPUT buffer\n");
    return false;
  }

  size_t copy_size = std::min(size, buf_in[buf_idx].len);
  memcpy(buf_in[buf_idx].addr, data, copy_size);

  // Dump first bytes of first few packets to verify stream format
  if (feed_call_count <= 3) {
    LOG_DEBUG("feed #%d: first 16 bytes:", feed_call_count);
    // Check for Annex B start code (00 00 00 01) or HVCC length prefix
    if (data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 && data[3] == 0x01) {
      LOG_DEBUG("feed #%d: Annex B format detected (start code)", feed_call_count);
    } else {
      LOG_DEBUG("feed #%d: NOT Annex B - may be HVCC/length-prefixed", feed_call_count);
    }
  }

  buf_in[buf_idx].sync(VISIONBUF_SYNC_TO_DEVICE);

  // Send extradata (VPS/SPS/PPS) with CODECCONFIG flag before first frame
  uint32_t flags = 0;
  if (!extradata.empty() && !extradata_sent) {
    // Prepend extradata to first frame if it fits, or send separately
    if (copy_size + extradata.size() <= buf_in[buf_idx].len) {
      memmove((uint8_t*)buf_in[buf_idx].addr + extradata.size(), buf_in[buf_idx].addr, copy_size);
      memcpy(buf_in[buf_idx].addr, extradata.data(), extradata.size());
      copy_size += extradata.size();
      flags = V4L2_QCOM_BUF_FLAG_CODECCONFIG;
      extradata_sent = true;
      LOG_DEBUG("feed #%d: prepended extradata (%zu bytes) with CODECCONFIG flag", feed_call_count, extradata.size());
    }
  }

  queueOutputBuffer(buf_idx, (uint32_t)copy_size, flags);

  LOG_DEBUG("feed: queued %zu bytes to OUTPUT buf %d flags=0x%x", copy_size, buf_idx, flags);

  // After feeding, check for SOURCE_CHANGE or proactively set up CAPTURE
  if (!capture_ready) {
    struct pollfd pfd = {.fd = fd, .events = POLLPRI, .revents = 0};
    int src_rc = poll(&pfd, 1, 100);
    LOG_DEBUG("feed #%d: post-feed SOURCE_CHANGE poll rc=%d revents=0x%x",
            feed_call_count, src_rc, pfd.revents);
    if (src_rc > 0 && (pfd.revents & POLLPRI)) {
      struct v4l2_event ev = {};
      if (util::safe_ioctl(fd, VIDIOC_DQEVENT, &ev) == 0 && ev.type == V4L2_EVENT_SOURCE_CHANGE) {
        struct v4l2_event_src_change *sc = (struct v4l2_event_src_change *)ev.u.data;
        fprintf(stderr, "[V4LDecoder] SOURCE_CHANGE after feed! changes=0x%x\n", sc->changes);
        setupCapture();
      }
    }

    // If still not ready after a few packets, try proactive CAPTURE setup.
    // Venus may not emit SOURCE_CHANGE until CAPTURE is configured.
    if (!capture_ready && feed_call_count >= 2) {
      fprintf(stderr, "[V4LDecoder] feed #%d: Proactively trying CAPTURE setup (no SOURCE_CHANGE)\n",
              feed_call_count);
      setupCapture();
    }
  }

  LOG_DEBUG("feed #%d: done, capture_ready=%d", feed_call_count, capture_ready);
  return true;
}

bool V4LDecoder::getFrame(VisionBuf *out_buf) {
  if (!is_open) {
    fprintf(stderr, "[V4LDecoder] getFrame: not open!\n");
    return false;
  }

  static int getframe_call_count = 0;
  getframe_call_count++;
  LOG_DEBUG("getFrame #%d: capture_ready=%d, out_buf=%p, stride=%d",
          getframe_call_count, capture_ready, out_buf, out_buf ? (int)out_buf->stride : -1);

  // If CAPTURE not ready, wait for SOURCE_CHANGE
  if (!capture_ready) {
    fprintf(stderr, "[V4LDecoder] getFrame #%d: waiting for CAPTURE setup (up to 10s)...\n", getframe_call_count);
    struct pollfd pfd = {.fd = fd, .events = POLLPRI | POLLOUT, .revents = 0};
    for (int waited = 0; waited < 10000; waited += 500) {
      int rc = poll(&pfd, 1, 500);
      LOG_DEBUG("getFrame #%d: SOURCE_CHANGE wait poll rc=%d waited=%dms revents=0x%x",
              getframe_call_count, rc, waited, pfd.revents);
      if (rc > 0) {
        if (pfd.revents & POLLPRI) {
          struct v4l2_event ev = {};
          if (util::safe_ioctl(fd, VIDIOC_DQEVENT, &ev) == 0 && ev.type == V4L2_EVENT_SOURCE_CHANGE) {
            struct v4l2_event_src_change *sc = (struct v4l2_event_src_change *)ev.u.data;
            fprintf(stderr, "[V4LDecoder] SOURCE_CHANGE in getFrame! changes=0x%x\n", sc->changes);
            if (setupCapture()) break;
          }
        }
        if (pfd.revents & POLLOUT) {
          v4l2_plane plane = {};
          v4l2_buffer v4l_buf = {
            .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
            .memory = V4L2_MEMORY_USERPTR,
            .m = { .planes = &plane, },
            .length = 1,
          };
          if (try_ioctl(fd, VIDIOC_DQBUF, &v4l_buf) == 0) {
            free_input_bufs.push(v4l_buf.index);
          }
        }
      }
    }
    if (!capture_ready) {
      fprintf(stderr, "[V4LDecoder] getFrame #%d: SOURCE_CHANGE TIMEOUT after 10s! Giving up.\n", getframe_call_count);
      return false;
    }
  }

  if (!capture_ready) {
    fprintf(stderr, "[V4LDecoder] getFrame #%d: still not capture_ready, returning false\n", getframe_call_count);
    return false;
  }

  LOG_DEBUG("getFrame #%d: polling for CAPTURE buffer (500ms timeout)...", getframe_call_count);
  struct pollfd pfd = {.fd = fd, .events = POLLIN | POLLOUT | POLLPRI, .revents = 0};
  int rc = poll(&pfd, 1, 500);
  LOG_DEBUG("getFrame #%d: poll rc=%d revents=0x%x (POLLIN=%d POLLOUT=%d POLLPRI=%d)",
          getframe_call_count, rc, pfd.revents,
          !!(pfd.revents & POLLIN), !!(pfd.revents & POLLOUT), !!(pfd.revents & POLLPRI));
  if (pfd.revents & POLLERR) {
    LOG_DEBUG("V4LDecoder::getFrame: poll error");
    return false;
  }
  if (pfd.revents & POLLHUP) {
    LOG_DEBUG("V4LDecoder::getFrame: poll hangup");
    return false;
  }
  if (pfd.revents & POLLNVAL) {
    LOG_DEBUG("V4LDecoder::getFrame: poll invalid");
    return false;
  }
  if (pfd.revents == 0) {
    LOG_DEBUG("V4LDecoder::getFrame: poll timeout after %d ms", 500);
    return false;
  }
  if (rc <= 0) {
    fprintf(stderr, "[V4LDecoder] getFrame #%d: poll timeout or error (rc=%d)\n", getframe_call_count, rc);
    return false;
  }

  if (pfd.revents & POLLPRI) {
    struct v4l2_event ev = {};
    util::safe_ioctl(fd, VIDIOC_DQEVENT, &ev);
    LOG_DEBUG("getFrame #%d: dequeued event type=%d", getframe_call_count, ev.type);
  }
  if (pfd.revents & POLLOUT) {
    v4l2_plane plane = {};
    v4l2_buffer v4l_buf = {
      .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
      .memory = V4L2_MEMORY_USERPTR,
      .m = { .planes = &plane, },
      .length = 1,
    };
    if (try_ioctl(fd, VIDIOC_DQBUF, &v4l_buf) == 0) {
      free_input_bufs.push(v4l_buf.index);
    }
  }
  if (!(pfd.revents & POLLIN)) {
    fprintf(stderr, "[V4LDecoder] getFrame #%d: no POLLIN, returning false\n", getframe_call_count);
    return false;
  }

  v4l2_plane plane = {};
  v4l2_buffer v4l_buf = {
    .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
    .memory = V4L2_MEMORY_USERPTR,
    .m = { .planes = &plane, },
    .length = 1,
  };
  if (try_ioctl(fd, VIDIOC_DQBUF, &v4l_buf) != 0) {
    fprintf(stderr, "[V4LDecoder] getFrame #%d: DQBUF CAPTURE failed\n", getframe_call_count);
    return false;
  }

  LOG_DEBUG("getFrame #%d: CAPTURE DQBUF idx=%d bytesused=%d flags=0x%x",
          getframe_call_count, v4l_buf.index, v4l_buf.m.planes[0].bytesused, v4l_buf.flags);

  if (v4l_buf.flags & (V4L2_QCOM_BUF_FLAG_CODECCONFIG | V4L2_QCOM_BUF_FLAG_EOS)) {
    LOG_DEBUG("getFrame #%d: skipping CODECCONFIG/EOS buffer (flags=0x%x)",
            getframe_call_count, v4l_buf.flags);
    queueCaptureBuffer(v4l_buf.index);
    return false;
  }

  buf_out[v4l_buf.index].sync(VISIONBUF_SYNC_FROM_DEVICE);

  uint8_t *src = (uint8_t *)buf_out[v4l_buf.index].addr;
  int src_stride = decoded_stride;
  int dst_stride = (int)out_buf->stride;
  if (dst_stride == 0) dst_stride = width;

  LOG_DEBUG("getFrame #%d: copying NV12 src_stride=%d dst_stride=%d %dx%d",
          getframe_call_count, src_stride, dst_stride, width, height);

  for (int i = 0; i < height; i++) {
    memcpy(out_buf->y + i * dst_stride, src + i * src_stride, width);
  }

  int uv_src_offset = src_stride * VENUS_Y_SCANLINES(COLOR_FMT_NV12, height);
  for (int i = 0; i < height / 2; i++) {
    memcpy(out_buf->uv + i * dst_stride, src + uv_src_offset + i * src_stride, width);
  }

  queueCaptureBuffer(v4l_buf.index);
  LOG_DEBUG("getFrame #%d: SUCCESS!", getframe_call_count);
  return true;
}

void V4LDecoder::flush() {
  if (!is_open) return;

  struct v4l2_decoder_cmd cmd = {.cmd = V4L2_DEC_CMD_STOP, .flags = 0};
  util::safe_ioctl(fd, VIDIOC_DECODER_CMD, &cmd);

  struct pollfd pfd = {.fd = fd, .events = POLLIN | POLLOUT, .revents = 0};
  while (poll(&pfd, 1, 100) > 0) {
    if (pfd.revents & POLLIN && capture_ready) {
      v4l2_plane plane = {};
      v4l2_buffer v4l_buf = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
        .memory = V4L2_MEMORY_USERPTR,
        .m = { .planes = &plane, },
        .length = 1,
      };
      if (try_ioctl(fd, VIDIOC_DQBUF, &v4l_buf) != 0) break;
      queueCaptureBuffer(v4l_buf.index);
    }
    if (pfd.revents & POLLOUT) {
      v4l2_plane plane = {};
      v4l2_buffer v4l_buf = {
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
        .memory = V4L2_MEMORY_USERPTR,
        .m = { .planes = &plane, },
        .length = 1,
      };
      if (try_ioctl(fd, VIDIOC_DQBUF, &v4l_buf) != 0) break;
      free_input_bufs.push(v4l_buf.index);
    }
  }

  cmd.cmd = V4L2_DEC_CMD_START;
  util::safe_ioctl(fd, VIDIOC_DECODER_CMD, &cmd);
}

void V4LDecoder::close() {
  if (!is_open) return;

  v4l2_buf_type buf_type = {};
  buf_type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  checked_ioctl(fd, VIDIOC_STREAMOFF, &buf_type);
  if (capture_ready) {
    buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    checked_ioctl(fd, VIDIOC_STREAMOFF, &buf_type);
  }

  request_buffers(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, 0, V4L2_MEMORY_USERPTR);
  if (capture_ready) request_buffers(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, 0, V4L2_MEMORY_USERPTR);

  for (int i = 0; i < V4L_DEC_BUF_IN_COUNT; i++) buf_in[i].free();
  if (capture_ready) {
    for (int i = 0; i < V4L_DEC_BUF_OUT_COUNT; i++) buf_out[i].free();
  }

  ::close(fd);
  fd = -1;
  is_open = false;
  capture_ready = false;
  extradata_sent = false;
  extradata.clear();
  fprintf(stderr, "[V4LDecoder] Closed\n");
}

V4LDecoder::V4LDecoder() {}
V4LDecoder::~V4LDecoder() { close(); }

#endif // QCOM2
