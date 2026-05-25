// v4l_decoder.cc - V4L2 hardware decoder for Qualcomm Venus (msm_vidc_vdec)
//
// Two-phase initialization:
//   Phase 1 (open): Set OUTPUT format, REQBUFS, STREAMON
//   Phase 2 (auto in feed): After first keyframe is fed, wait for SOURCE_CHANGE,
//     then set up CAPTURE: G_FMT, REQBUFS, allocate ION, STREAMON, queue buffers
//
// Device: /dev/v4l/by-path/platform-aa00000.qcom_vidc-video-index0
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

const int env_debug_decoder = (getenv("DEBUG_V4L_DECODER") != NULL) ? atoi(getenv("DEBUG_V4L_DECODER")) : 0;

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

static void queue_buffer(int fd, v4l2_buf_type buf_type, unsigned int index,
                          VisionBuf *buf, uint32_t bytesused, struct timeval timestamp = {}) {
  v4l2_plane plane = {
    .length = (unsigned int)buf->len,
    .m = { .userptr = (unsigned long)buf->addr, },
    .bytesused = bytesused,
    .reserved = {(unsigned int)buf->fd}
  };

  v4l2_buffer v4l_buf = {
    .type = buf_type,
    .index = index,
    .memory = V4L2_MEMORY_USERPTR,
    .m = { .planes = &plane, },
    .length = 1,
    .flags = V4L2_BUF_FLAG_TIMESTAMP_COPY,
    .timestamp = timestamp
  };

  checked_ioctl(fd, VIDIOC_QBUF, &v4l_buf);
}

static bool request_buffers(int fd, v4l2_buf_type buf_type, unsigned int count) {
  struct v4l2_requestbuffers reqbuf = {
    .type = buf_type,
    .memory = V4L2_MEMORY_USERPTR,
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

  fd = ::open("/dev/v4l/by-path/platform-aa00000.qcom_vidc-video-index0", O_RDWR | O_NONBLOCK);
  if (fd < 0) {
    fprintf(stderr, "[V4LDecoder] Failed to open decoder device: %s\n", strerror(errno));
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
    fprintf(stderr, "[V4LDecoder] Subscribed to V4L2_EVENT_SOURCE_CHANGE\n");
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

  // Set CAPTURE format (decoded NV12 output) upfront.
  // msm_vidc (Venus) requires CAPTURE format to be set before processing data.
  // Unlike standard V4L2 M2M, Venus won't emit SOURCE_CHANGE without this.
  decoded_stride = VENUS_Y_STRIDE(COLOR_FMT_NV12, width);
  output_buf_size = (size_t)VENUS_BUFFER_SIZE(COLOR_FMT_NV12, width, height);

  fmt = {};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  fmt.fmt.pix_mp.width = (unsigned int)width;
  fmt.fmt.pix_mp.height = (unsigned int)height;
  fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
  fmt.fmt.pix_mp.field = V4L2_FIELD_ANY;
  fmt.fmt.pix_mp.num_planes = 1;
  fmt.fmt.pix_mp.plane_fmt[0].sizeimage = (unsigned int)output_buf_size;

  if (!checked_ioctl(fd, VIDIOC_S_FMT, &fmt)) {
    fprintf(stderr, "[V4LDecoder] CAPTURE S_FMT failed, trying without\n");
    // Non-fatal: some drivers may reject upfront CAPTURE format
  } else {
    fprintf(stderr, "[V4LDecoder] CAPTURE format: NV12 %dx%d, sizeimage=%d\n",
            fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
            fmt.fmt.pix_mp.plane_fmt[0].sizeimage);
    // Update sizes from actual format
    output_buf_size = (size_t)fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
  }

  // Allocate ION buffers for OUTPUT
  for (int i = 0; i < V4L_DEC_BUF_IN_COUNT; i++) {
    buf_in[i].allocate(input_buf_size);
    free_input_bufs.push(i);
  }

  // Allocate ION buffers for CAPTURE
  for (int i = 0; i < V4L_DEC_BUF_OUT_COUNT; i++) {
    buf_out[i].allocate(output_buf_size);
  }

  // Request OUTPUT buffers
  if (!request_buffers(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L_DEC_BUF_IN_COUNT)) goto fail;
  fprintf(stderr, "[V4LDecoder] OUTPUT REQBUFS: %d buffers\n", V4L_DEC_BUF_IN_COUNT);

  // Request CAPTURE buffers
  if (!request_buffers(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L_DEC_BUF_OUT_COUNT)) {
    fprintf(stderr, "[V4LDecoder] CAPTURE REQBUFS failed\n");
    goto fail;
  }
  fprintf(stderr, "[V4LDecoder] CAPTURE REQBUFS: %d buffers\n", V4L_DEC_BUF_OUT_COUNT);

  // Start CAPTURE streaming first (Venus requirement)
  buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  if (!checked_ioctl(fd, VIDIOC_STREAMON, &buf_type)) {
    fprintf(stderr, "[V4LDecoder] CAPTURE STREAMON failed\n");
    goto fail;
  }
  fprintf(stderr, "[V4LDecoder] CAPTURE STREAMON successful\n");

  // Queue all CAPTURE buffers
  for (int i = 0; i < V4L_DEC_BUF_OUT_COUNT; i++) {
    queueCaptureBuffer(i);
  }
  fprintf(stderr, "[V4LDecoder] Queued %d CAPTURE buffers\n", V4L_DEC_BUF_OUT_COUNT);

  // Start OUTPUT streaming
  buf_type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  if (!checked_ioctl(fd, VIDIOC_STREAMON, &buf_type)) goto fail;
  fprintf(stderr, "[V4LDecoder] OUTPUT STREAMON successful\n");

  is_open = true;
  capture_ready = true;
  fprintf(stderr, "[V4LDecoder] Initialization complete. OUTPUT+CAPTURE ready (%dx%d, stride=%d, buf=%zu)\n",
          width, height, decoded_stride, output_buf_size);
  return true;

fail:
  // Cleanup OUTPUT buffers
  for (int i = 0; i < V4L_DEC_BUF_IN_COUNT; i++) {
    buf_in[i].free();
  }
  while (!free_input_bufs.empty()) free_input_bufs.pop();
  // Cleanup CAPTURE buffers (may not have been allocated if fail early)
  for (int i = 0; i < V4L_DEC_BUF_OUT_COUNT; i++) {
    buf_out[i].free();
  }
  ::close(fd);
  fd = -1;
  fprintf(stderr, "[V4LDecoder] open failed, falling back to CPU decoder\n");
  return false;
}

bool V4LDecoder::setupCapture() {
  struct v4l2_format fmt = {};
  v4l2_buf_type buf_type = (v4l2_buf_type)0;

  fprintf(stderr, "[V4LDecoder] Setting up CAPTURE after SOURCE_CHANGE...\n");

  // Get actual CAPTURE format
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

  // Allocate CAPTURE ION buffers
  for (int i = 0; i < V4L_DEC_BUF_OUT_COUNT; i++) {
    buf_out[i].allocate(output_buf_size);
  }

  // Request CAPTURE buffers
  if (!request_buffers(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L_DEC_BUF_OUT_COUNT)) {
    fprintf(stderr, "[V4LDecoder] CAPTURE REQBUFS failed\n");
    for (int i = 0; i < V4L_DEC_BUF_OUT_COUNT; i++) buf_out[i].free();
    return false;
  }
  fprintf(stderr, "[V4LDecoder] CAPTURE REQBUFS: %d buffers\n", V4L_DEC_BUF_OUT_COUNT);

  // Start CAPTURE streaming
  buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  if (!checked_ioctl(fd, VIDIOC_STREAMON, &buf_type)) {
    fprintf(stderr, "[V4LDecoder] CAPTURE STREAMON failed\n");
    for (int i = 0; i < V4L_DEC_BUF_OUT_COUNT; i++) buf_out[i].free();
    return false;
  }
  fprintf(stderr, "[V4LDecoder] CAPTURE STREAMON successful\n");

  // Queue all CAPTURE buffers
  for (int i = 0; i < V4L_DEC_BUF_OUT_COUNT; i++) {
    queueCaptureBuffer(i);
  }
  fprintf(stderr, "[V4LDecoder] Queued %d CAPTURE buffers\n", V4L_DEC_BUF_OUT_COUNT);

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
  queue_buffer(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, index, &buf_out[index], (uint32_t)buf_out[index].len);
}

void V4LDecoder::queueOutputBuffer(int index, uint32_t bytesused) {
  struct timeval ts = {};
  queue_buffer(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, index, &buf_in[index], bytesused, ts);
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

bool V4LDecoder::feed(const uint8_t *data, size_t size) {
  if (!is_open) {
    fprintf(stderr, "[V4LDecoder] feed: not open!\n");
    return false;
  }

  static int feed_call_count = 0;
  feed_call_count++;
  fprintf(stderr, "[V4LDecoder] feed #%d: size=%zu, capture_ready=%d\n",
          feed_call_count, size, capture_ready);

  drainOutput();

  // Check for SOURCE_CHANGE event (non-blocking)
  if (!capture_ready) {
    struct pollfd pfd = {.fd = fd, .events = POLLPRI | POLLOUT, .revents = 0};
    int rc = poll(&pfd, 1, 0);
    fprintf(stderr, "[V4LDecoder] feed #%d: SOURCE_CHANGE poll rc=%d revents=0x%x\n",
            feed_call_count, rc, pfd.revents);
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
        fprintf(stderr, "[V4LDecoder] feed #%d: POLLPRI but event type=%d (not SOURCE_CHANGE=%d)\n",
                feed_call_count, ev.type, V4L2_EVENT_SOURCE_CHANGE);
      }
    }
  }

  // Also drain OUTPUT after poll
  if (!capture_ready) {
    // Still waiting for SOURCE_CHANGE - drain OUTPUT and continue
    fprintf(stderr, "[V4LDecoder] feed #%d: capture not ready, draining output\n", feed_call_count);
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
  buf_in[buf_idx].sync(VISIONBUF_SYNC_TO_DEVICE);
  queueOutputBuffer(buf_idx, (uint32_t)copy_size);

  if (env_debug_decoder) {
    printf("[V4LDecoder] feed: queued %zu bytes to OUTPUT buf %d\n", copy_size, buf_idx);
  }

  // After feeding, check again for SOURCE_CHANGE
  if (!capture_ready) {
    struct pollfd pfd = {.fd = fd, .events = POLLPRI, .revents = 0};
    int src_rc = poll(&pfd, 1, 100);
    fprintf(stderr, "[V4LDecoder] feed #%d: post-feed SOURCE_CHANGE poll rc=%d revents=0x%x\n",
            feed_call_count, src_rc, pfd.revents);
    if (src_rc > 0 && (pfd.revents & POLLPRI)) {
      struct v4l2_event ev = {};
      if (util::safe_ioctl(fd, VIDIOC_DQEVENT, &ev) == 0 && ev.type == V4L2_EVENT_SOURCE_CHANGE) {
        struct v4l2_event_src_change *sc = (struct v4l2_event_src_change *)ev.u.data;
        fprintf(stderr, "[V4LDecoder] SOURCE_CHANGE after feed! changes=0x%x\n", sc->changes);
        setupCapture();
      }
    } else if (feed_call_count <= 5) {
      fprintf(stderr, "[V4LDecoder] feed #%d: still no SOURCE_CHANGE after 100ms (capture_ready=%d)\n",
              feed_call_count, capture_ready);
    }
  }

  fprintf(stderr, "[V4LDecoder] feed #%d: done, capture_ready=%d\n", feed_call_count, capture_ready);
  return true;
}

bool V4LDecoder::getFrame(VisionBuf *out_buf) {
  if (!is_open) {
    fprintf(stderr, "[V4LDecoder] getFrame: not open!\n");
    return false;
  }

  static int getframe_call_count = 0;
  getframe_call_count++;
  fprintf(stderr, "[V4LDecoder] getFrame #%d: capture_ready=%d, out_buf=%p, stride=%d\n",
          getframe_call_count, capture_ready, out_buf, out_buf ? (int)out_buf->stride : -1);

  // If CAPTURE not ready, wait for SOURCE_CHANGE
  if (!capture_ready) {
    fprintf(stderr, "[V4LDecoder] getFrame #%d: waiting for CAPTURE setup (up to 10s)...\n", getframe_call_count);
    struct pollfd pfd = {.fd = fd, .events = POLLPRI | POLLOUT, .revents = 0};
    for (int waited = 0; waited < 10000; waited += 500) {
      int rc = poll(&pfd, 1, 500);
      fprintf(stderr, "[V4LDecoder] getFrame #%d: SOURCE_CHANGE wait poll rc=%d waited=%dms revents=0x%x\n",
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

  fprintf(stderr, "[V4LDecoder] getFrame #%d: polling for CAPTURE buffer (500ms timeout)...\n", getframe_call_count);
  struct pollfd pfd = {.fd = fd, .events = POLLIN | POLLOUT | POLLPRI, .revents = 0};
  int rc = poll(&pfd, 1, 500);
  fprintf(stderr, "[V4LDecoder] getFrame #%d: poll rc=%d revents=0x%x (POLLIN=%d POLLOUT=%d POLLPRI=%d)\n",
          getframe_call_count, rc, pfd.revents,
          !!(pfd.revents & POLLIN), !!(pfd.revents & POLLOUT), !!(pfd.revents & POLLPRI));
  if (rc <= 0) {
    fprintf(stderr, "[V4LDecoder] getFrame #%d: poll timeout or error (rc=%d)\n", getframe_call_count, rc);
    return false;
  }

  if (pfd.revents & POLLPRI) {
    struct v4l2_event ev = {};
    util::safe_ioctl(fd, VIDIOC_DQEVENT, &ev);
    fprintf(stderr, "[V4LDecoder] getFrame #%d: dequeued event type=%d\n", getframe_call_count, ev.type);
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

  fprintf(stderr, "[V4LDecoder] getFrame #%d: CAPTURE DQBUF idx=%d bytesused=%d flags=0x%x\n",
          getframe_call_count, v4l_buf.index, v4l_buf.m.planes[0].bytesused, v4l_buf.flags);

  if (v4l_buf.flags & (V4L2_QCOM_BUF_FLAG_CODECCONFIG | V4L2_QCOM_BUF_FLAG_EOS)) {
    fprintf(stderr, "[V4LDecoder] getFrame #%d: skipping CODECCONFIG/EOS buffer (flags=0x%x)\n",
            getframe_call_count, v4l_buf.flags);
    queueCaptureBuffer(v4l_buf.index);
    return false;
  }

  buf_out[v4l_buf.index].sync(VISIONBUF_SYNC_FROM_DEVICE);

  uint8_t *src = (uint8_t *)buf_out[v4l_buf.index].addr;
  int src_stride = decoded_stride;
  int dst_stride = (int)out_buf->stride;
  if (dst_stride == 0) dst_stride = width;

  fprintf(stderr, "[V4LDecoder] getFrame #%d: copying NV12 src_stride=%d dst_stride=%d %dx%d\n",
          getframe_call_count, src_stride, dst_stride, width, height);

  for (int i = 0; i < height; i++) {
    memcpy(out_buf->y + i * dst_stride, src + i * src_stride, width);
  }

  int uv_src_offset = src_stride * VENUS_Y_SCANLINES(COLOR_FMT_NV12, height);
  for (int i = 0; i < height / 2; i++) {
    memcpy(out_buf->uv + i * dst_stride, src + uv_src_offset + i * src_stride, width);
  }

  queueCaptureBuffer(v4l_buf.index);
  fprintf(stderr, "[V4LDecoder] getFrame #%d: SUCCESS!\n", getframe_call_count);
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

  request_buffers(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, 0);
  if (capture_ready) request_buffers(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, 0);

  for (int i = 0; i < V4L_DEC_BUF_IN_COUNT; i++) buf_in[i].free();
  if (capture_ready) {
    for (int i = 0; i < V4L_DEC_BUF_OUT_COUNT; i++) buf_out[i].free();
  }

  ::close(fd);
  fd = -1;
  is_open = false;
  capture_ready = false;
  fprintf(stderr, "[V4LDecoder] Closed\n");
}

V4LDecoder::V4LDecoder() {}
V4LDecoder::~V4LDecoder() { close(); }

#endif // QCOM2