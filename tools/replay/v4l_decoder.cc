// v4l_decoder.cc - V4L2 hardware decoder for Qualcomm Venus (msm_vidc_vdec)
//
// Uses SOURCE_CHANGE event flow for dynamic resolution negotiation:
//   1. Open device, subscribe to V4L2_EVENT_SOURCE_CHANGE
//   2. Set OUTPUT format (HEVC), REQBUFS, STREAMON
//   3. Feed first keyframe to OUTPUT
//   4. Wait for SOURCE_CHANGE event (V4L2_EVENT_SRC_CH_RESOLUTION_CHANGED)
//   5. Get CAPTURE format from driver (G_FMT)
//   6. Allocate CAPTURE buffers, REQBUFS, STREAMON, queue empty buffers
//   7. Normal decode loop: OUTPUT QBUF → CAPTURE DQBUF
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

// Returns 0 on success, -1 on error, 1 on EAGAIN (would block)
static int try_ioctl(int fd, unsigned long request, void *argp) {
  int ret = util::safe_ioctl(fd, request, argp);
  if (ret != 0) {
    if (errno == EAGAIN) return 1;
    if (env_debug_decoder) {
      fprintf(stderr, "[V4LDecoder] try_ioctl: fd=%d request=0x%lx errno=%d (%s)\n",
              fd, request, errno, strerror(errno));
    }
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

  // Declare all variables upfront to avoid C++ goto-bypasses-initialization errors
  struct v4l2_capability cap = {};
  struct v4l2_format fmt_out = {};
  v4l2_buf_type buf_type = (v4l2_buf_type)0;
  bool ion_allocated = false;

  fd = ::open("/dev/v4l/by-path/platform-aa00000.qcom_vidc-video-index0", O_RDWR | O_NONBLOCK);
  if (fd < 0) {
    fprintf(stderr, "[V4LDecoder] Failed to open decoder device: %s\n", strerror(errno));
    return false;
  }

  // Verify device
  if (!checked_ioctl(fd, VIDIOC_QUERYCAP, &cap)) {
    fprintf(stderr, "[V4LDecoder] QUERYCAP failed\n");
    goto fail_close_fd;
  }
  if (strcmp((const char *)cap.driver, "msm_vidc_driver") != 0 ||
      strcmp((const char *)cap.card, "msm_vidc_vdec") != 0) {
    fprintf(stderr, "[V4LDecoder] Wrong device: driver=%s card=%s\n", cap.driver, cap.card);
    goto fail_close_fd;
  }
  fprintf(stderr, "[V4LDecoder] Opened decoder: %s %s fd=%d\n", cap.driver, cap.card, fd);

  // Subscribe to SOURCE_CHANGE event (required by Venus/msm_vidc decoder)
  {
    struct v4l2_event_subscription sub = {};
    sub.type = V4L2_EVENT_SOURCE_CHANGE;
    if (!checked_ioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub)) {
      fprintf(stderr, "[V4LDecoder] Failed to subscribe to SOURCE_CHANGE event\n");
      goto fail_close_fd;
    }
    fprintf(stderr, "[V4LDecoder] Subscribed to V4L2_EVENT_SOURCE_CHANGE\n");
  }

  // Set OUTPUT format (compressed HEVC input)
  fmt_out = {
    .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
    .fmt = {
      .pix_mp = {
        .width = (unsigned int)width,
        .height = (unsigned int)height,
        .pixelformat = V4L2_PIX_FMT_HEVC,
        .field = V4L2_FIELD_ANY,
        .colorspace = V4L2_COLORSPACE_DEFAULT,
        .num_planes = 1,
        .plane_fmt = {
          [0] = { .sizeimage = 2 * 1024 * 1024 }  // max compressed frame size
        }
      }
    }
  };
  if (!checked_ioctl(fd, VIDIOC_S_FMT, &fmt_out)) {
    fprintf(stderr, "[V4LDecoder] Failed to set OUTPUT format\n");
    goto fail_close_fd;
  }
  input_buf_size = fmt_out.fmt.pix_mp.plane_fmt[0].sizeimage;
  fprintf(stderr, "[V4LDecoder] OUTPUT format: HEVC %dx%d, sizeimage=%zu\n",
          fmt_out.fmt.pix_mp.width, fmt_out.fmt.pix_mp.height, input_buf_size);

  // Allocate ION buffers for OUTPUT (compressed input)
  for (int i = 0; i < V4L_DEC_BUF_IN_COUNT; i++) {
    buf_in[i].allocate(input_buf_size);
    free_input_bufs.push(i);
  }
  ion_allocated = true;

  // Request OUTPUT buffers only (CAPTURE buffers allocated after SOURCE_CHANGE)
  if (!request_buffers(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L_DEC_BUF_IN_COUNT)) {
    fprintf(stderr, "[V4LDecoder] Failed to request OUTPUT buffers\n");
    goto fail_free_ion;
  }
  fprintf(stderr, "[V4LDecoder] OUTPUT REQBUFS: %d buffers\n", V4L_DEC_BUF_IN_COUNT);

  // Start OUTPUT streaming only (CAPTURE starts after SOURCE_CHANGE)
  buf_type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  if (!checked_ioctl(fd, VIDIOC_STREAMON, &buf_type)) {
    fprintf(stderr, "[V4LDecoder] Failed to start OUTPUT streaming\n");
    goto fail_free_ion;
  }
  fprintf(stderr, "[V4LDecoder] OUTPUT STREAMON successful (CAPTURE deferred until SOURCE_CHANGE)\n");

  // NOTE: CAPTURE setup is deferred until waitForSourceChange() is called from feed()
  // This is the Venus/msm_vidc requirement: the driver needs to parse the first
  // compressed frame before it can determine the output resolution.

  is_open = true;
  fprintf(stderr, "[V4LDecoder] OUTPUT ready: %dx%d, waiting for first frame to trigger SOURCE_CHANGE\n",
          width, height);
  return true;

fail_free_ion:
  if (ion_allocated) {
    for (int i = 0; i < V4L_DEC_BUF_IN_COUNT; i++) {
      buf_in[i].free();
    }
    while (!free_input_bufs.empty()) {
      free_input_bufs.pop();
    }
  }
fail_close_fd:
  ::close(fd);
  fd = -1;
  fprintf(stderr, "[V4LDecoder] open failed, falling back to CPU decoder\n");
  return false;
}

bool V4LDecoder::waitForSourceChange() {
  fprintf(stderr, "[V4LDecoder] Waiting for SOURCE_CHANGE event...\n");

  // Poll for events with timeout
  // POLLPRI is used for V4L2 events, POLLOUT for OUTPUT DQBUF
  int max_attempts = 100;  // 100 * 100ms = 10 seconds max
  for (int attempt = 0; attempt < max_attempts; attempt++) {
    struct pollfd pfd = {
      .fd = fd,
      .events = POLLPRI | POLLOUT,
      .revents = 0
    };
    int rc = poll(&pfd, 1, 100);  // 100ms per poll

    if (rc < 0) {
      if (errno == EINTR) continue;
      fprintf(stderr, "[V4LDecoder] poll error during SOURCE_CHANGE wait: %s\n", strerror(errno));
      return false;
    }

    if (rc == 0) {
      // Timeout - keep waiting
      if (attempt % 10 == 9) {
        fprintf(stderr, "[V4LDecoder] Still waiting for SOURCE_CHANGE... (%d/100)\n", attempt + 1);
      }
      continue;
    }

    // Drain OUTPUT buffers (free completed input buffers)
    if (pfd.revents & POLLOUT) {
      v4l2_plane plane = {};
      v4l2_buffer v4l_buf = {
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
        .memory = V4L2_MEMORY_USERPTR,
        .m = { .planes = &plane, },
        .length = 1,
      };
      int ret = try_ioctl(fd, VIDIOC_DQBUF, &v4l_buf);
      if (ret == 0) {
        free_input_bufs.push(v4l_buf.index);
        if (env_debug_decoder) {
          printf("[V4LDecoder] OUTPUT DQBUF during SOURCE_CHANGE wait: idx=%d\n", v4l_buf.index);
        }
      }
    }

    // Check for V4L2 events (SOURCE_CHANGE)
    if (pfd.revents & POLLPRI) {
      struct v4l2_event ev = {};
      if (!checked_ioctl(fd, VIDIOC_DQEVENT, &ev)) {
        fprintf(stderr, "[V4LDecoder] DQEVENT failed\n");
        return false;
      }

      fprintf(stderr, "[V4LDecoder] Received event: type=%d\n", ev.type);

      if (ev.type == V4L2_EVENT_SOURCE_CHANGE) {
        struct v4l2_event_src_change *sc = (struct v4l2_event_src_change *)ev.u.data;
        fprintf(stderr, "[V4LDecoder] SOURCE_CHANGE event: changes=0x%x\n", sc->changes);

        if (sc->changes & V4L2_EVENT_SRC_CH_RESOLUTION_CHANGED) {
          fprintf(stderr, "[V4LDecoder] Resolution changed detected, configuring CAPTURE stream\n");

          // Get CAPTURE format from driver (driver knows the real resolution now)
          struct v4l2_format fmt_cap = {};
          fmt_cap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
          if (!checked_ioctl(fd, VIDIOC_G_FMT, &fmt_cap)) {
            fprintf(stderr, "[V4LDecoder] Failed to get CAPTURE format\n");
            return false;
          }

          // Update actual decoded resolution from driver
          int drv_width = fmt_cap.fmt.pix_mp.width;
          int drv_height = fmt_cap.fmt.pix_mp.height;
          fprintf(stderr, "[V4LDecoder] Driver reports CAPTURE: %dx%d, pixelformat=0x%x, num_planes=%d, sizeimage=%d\n",
                  drv_width, drv_height,
                  fmt_cap.fmt.pix_mp.pixelformat,
                  fmt_cap.fmt.pix_mp.num_planes,
                  fmt_cap.fmt.pix_mp.plane_fmt[0].sizeimage);

          // Calculate stride and buffer size using Venus macros
          decoded_stride = VENUS_Y_STRIDE(COLOR_FMT_NV12, drv_width);
          output_buf_size = (size_t)VENUS_BUFFER_SIZE(COLOR_FMT_NV12, drv_width, drv_height);

          // Try S_FMT with driver-reported values (some drivers need this)
          // Use the sizeimage from G_FMT as the driver knows best
          fmt_cap.fmt.pix_mp.plane_fmt[0].sizeimage = output_buf_size;
          if (!checked_ioctl(fd, VIDIOC_S_FMT, &fmt_cap)) {
            fprintf(stderr, "[V4LDecoder] Warning: S_FMT on CAPTURE failed (continuing with G_FMT values)\n");
            // Not fatal - G_FMT already set the format
          }

          fprintf(stderr, "[V4LDecoder] CAPTURE: stride=%d, buffer_size=%zu\n",
                  decoded_stride, output_buf_size);

          // Allocate ION buffers for CAPTURE (decoded output)
          for (int i = 0; i < V4L_DEC_BUF_OUT_COUNT; i++) {
            buf_out[i].allocate(output_buf_size);
          }

          // Request CAPTURE buffers
          if (!request_buffers(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L_DEC_BUF_OUT_COUNT)) {
            fprintf(stderr, "[V4LDecoder] Failed to request CAPTURE buffers\n");
            // Free CAPTURE ION buffers
            for (int i = 0; i < V4L_DEC_BUF_OUT_COUNT; i++) {
              buf_out[i].free();
            }
            return false;
          }
          fprintf(stderr, "[V4LDecoder] CAPTURE REQBUFS: %d buffers\n", V4L_DEC_BUF_OUT_COUNT);

          // Start CAPTURE streaming
          v4l2_buf_type cap_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
          if (!checked_ioctl(fd, VIDIOC_STREAMON, &cap_type)) {
            fprintf(stderr, "[V4LDecoder] Failed to start CAPTURE streaming\n");
            return false;
          }
          fprintf(stderr, "[V4LDecoder] CAPTURE STREAMON successful\n");

          // Queue all CAPTURE buffers (empty, to be filled by decoder)
          for (int i = 0; i < V4L_DEC_BUF_OUT_COUNT; i++) {
            queueCaptureBuffer(i);
          }
          fprintf(stderr, "[V4LDecoder] Queued %d CAPTURE buffers\n", V4L_DEC_BUF_OUT_COUNT);

          capture_ready = true;
          fprintf(stderr, "[V4LDecoder] CAPTURE ready: %dx%d, stride=%d\n",
                  drv_width, drv_height, decoded_stride);
          return true;
        }
      }
    }
  }

  fprintf(stderr, "[V4LDecoder] Timed out waiting for SOURCE_CHANGE event\n");
  return false;
}

void V4LDecoder::drainOutput() {
  // Non-blocking drain of OUTPUT (consumed compressed data) buffers
  while (true) {
    v4l2_plane plane = {};
    v4l2_buffer v4l_buf = {
      .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
      .memory = V4L2_MEMORY_USERPTR,
      .m = { .planes = &plane, },
      .length = 1,
    };
    int ret = try_ioctl(fd, VIDIOC_DQBUF, &v4l_buf);
    if (ret != 0) break;
    free_input_bufs.push(v4l_buf.index);
    if (env_debug_decoder) {
      printf("[V4LDecoder] OUTPUT DQBUF: idx=%d (freed)\n", v4l_buf.index);
    }
  }
}

void V4LDecoder::queueCaptureBuffer(int index) {
  // Queue empty CAPTURE buffer for decoder to fill
  queue_buffer(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, index, &buf_out[index], (uint32_t)buf_out[index].len);
}

void V4LDecoder::queueOutputBuffer(int index, uint32_t bytesused) {
  struct timeval ts = {};
  queue_buffer(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, index, &buf_in[index], bytesused, ts);
}

void V4LDecoder::drainCapture() {
  if (!capture_ready) return;

  // Non-blocking drain of completed CAPTURE buffers
  struct pollfd pfd = {.fd = fd, .events = POLLIN | POLLOUT | POLLPRI, .revents = 0};

  while (true) {
    int rc = poll(&pfd, 1, 0);  // 0 = non-blocking
    if (rc <= 0) break;

    // Handle events (e.g., resolution change during playback)
    if (pfd.revents & POLLPRI) {
      struct v4l2_event ev = {};
      if (util::safe_ioctl(fd, VIDIOC_DQEVENT, &ev) == 0) {
        if (ev.type == V4L2_EVENT_SOURCE_CHANGE) {
          struct v4l2_event_src_change *sc = (struct v4l2_event_src_change *)ev.u.data;
          fprintf(stderr, "[V4LDecoder] SOURCE_CHANGE during playback: changes=0x%x\n", sc->changes);
          // For now we log it; resolution changes mid-stream are not expected in our use case
        }
      }
    }

    if (pfd.revents & POLLIN) {
      // Dequeue CAPTURE buffer (decoded frame)
      v4l2_plane plane = {};
      v4l2_buffer v4l_buf = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
        .memory = V4L2_MEMORY_USERPTR,
        .m = { .planes = &plane, },
        .length = 1,
      };
      int ret = try_ioctl(fd, VIDIOC_DQBUF, &v4l_buf);
      if (ret != 0) break;

      if (env_debug_decoder) {
        printf("[V4LDecoder] CAPTURE DQBUF: idx=%d bytesused=%d flags=0x%x\n",
               v4l_buf.index, v4l_buf.m.planes[0].bytesused, v4l_buf.flags);
      }

      // Re-queue the CAPTURE buffer
      queueCaptureBuffer(v4l_buf.index);
    }

    if (pfd.revents & POLLOUT) {
      // Dequeue OUTPUT buffer (consumed compressed data)
      v4l2_plane plane = {};
      v4l2_buffer v4l_buf = {
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
        .memory = V4L2_MEMORY_USERPTR,
        .m = { .planes = &plane, },
        .length = 1,
      };
      int ret = try_ioctl(fd, VIDIOC_DQBUF, &v4l_buf);
      if (ret != 0) break;

      free_input_bufs.push(v4l_buf.index);
      if (env_debug_decoder) {
        printf("[V4LDecoder] OUTPUT DQBUF: idx=%d (freed)\n", v4l_buf.index);
      }
    }
  }
}

bool V4LDecoder::feed(const uint8_t *data, size_t size) {
  if (!is_open) return false;

  // First feed: wait for SOURCE_CHANGE event to configure CAPTURE stream
  if (!capture_ready) {
    fprintf(stderr, "[V4LDecoder] First frame received (%zu bytes), triggering SOURCE_CHANGE\n", size);

    // Drain any completed OUTPUT buffers first
    drainOutput();

    // Get a free input buffer
    if (free_input_bufs.empty()) {
      fprintf(stderr, "[V4LDecoder] No OUTPUT buffers available for first frame\n");
      return false;
    }
    int buf_idx = free_input_bufs.pop();

    // Copy compressed data to ION buffer
    size_t copy_size = std::min(size, buf_in[buf_idx].len);
    memcpy(buf_in[buf_idx].addr, data, copy_size);
    buf_in[buf_idx].sync(VISIONBUF_SYNC_TO_DEVICE);

    // Queue the first frame to OUTPUT (this triggers SOURCE_CHANGE)
    queueOutputBuffer(buf_idx, (uint32_t)copy_size);
    fprintf(stderr, "[V4LDecoder] Queued first frame: %zu bytes to OUTPUT buf %d\n", copy_size, buf_idx);

    // Wait for SOURCE_CHANGE event and configure CAPTURE
    if (!waitForSourceChange()) {
      fprintf(stderr, "[V4LDecoder] SOURCE_CHANGE negotiation failed\n");
      return false;
    }

    return true;
  }

  // Normal path: CAPTURE is ready
  // Drain any completed buffers first
  drainCapture();

  // Get a free input buffer
  int buf_idx = -1;
  {
    // Try non-blocking first
    if (free_input_bufs.empty()) {
      // Wait for a buffer to become available
      struct pollfd pfd = {.fd = fd, .events = POLLOUT, .revents = 0};
      int rc = poll(&pfd, 1, 100);  // 100ms timeout
      if (rc <= 0) {
        fprintf(stderr, "[V4LDecoder] feed: no OUTPUT buffers available\n");
        return false;
      }
      // Drain OUTPUT DQBUF
      v4l2_plane plane = {};
      v4l2_buffer v4l_buf = {
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
        .memory = V4L2_MEMORY_USERPTR,
        .m = { .planes = &plane, },
        .length = 1,
      };
      int ret = try_ioctl(fd, VIDIOC_DQBUF, &v4l_buf);
      if (ret == 0) {
        free_input_bufs.push(v4l_buf.index);
      }
    }

    if (!free_input_bufs.empty()) {
      buf_idx = free_input_bufs.pop();
    }
  }

  if (buf_idx < 0) {
    fprintf(stderr, "[V4LDecoder] feed: failed to get OUTPUT buffer\n");
    return false;
  }

  // Copy compressed data to ION buffer
  size_t copy_size = std::min(size, buf_in[buf_idx].len);
  memcpy(buf_in[buf_idx].addr, data, copy_size);
  buf_in[buf_idx].sync(VISIONBUF_SYNC_TO_DEVICE);

  // Queue the OUTPUT buffer
  queueOutputBuffer(buf_idx, (uint32_t)copy_size);

  if (env_debug_decoder) {
    printf("[V4LDecoder] feed: queued %zu bytes to OUTPUT buf %d\n", copy_size, buf_idx);
  }

  return true;
}

bool V4LDecoder::getFrame(VisionBuf *out_buf) {
  if (!is_open || !capture_ready) return false;

  // Poll for decoded frame with timeout
  struct pollfd pfd = {.fd = fd, .events = POLLIN | POLLOUT | POLLPRI, .revents = 0};
  int rc = poll(&pfd, 1, 500);  // 500ms timeout

  if (rc <= 0) {
    if (env_debug_decoder) {
      fprintf(stderr, "[V4LDecoder] getFrame: poll timeout or error (rc=%d)\n", rc);
    }
    return false;
  }

  // Handle events
  if (pfd.revents & POLLPRI) {
    struct v4l2_event ev = {};
    if (util::safe_ioctl(fd, VIDIOC_DQEVENT, &ev) == 0) {
      if (ev.type == V4L2_EVENT_SOURCE_CHANGE) {
        struct v4l2_event_src_change *sc = (struct v4l2_event_src_change *)ev.u.data;
        fprintf(stderr, "[V4LDecoder] SOURCE_CHANGE during getFrame: changes=0x%x\n", sc->changes);
      }
    }
  }

  // Also drain OUTPUT buffers while we're here
  if (pfd.revents & POLLOUT) {
    v4l2_plane plane = {};
    v4l2_buffer v4l_buf = {
      .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
      .memory = V4L2_MEMORY_USERPTR,
      .m = { .planes = &plane, },
      .length = 1,
    };
    int ret = try_ioctl(fd, VIDIOC_DQBUF, &v4l_buf);
    if (ret == 0) {
      free_input_bufs.push(v4l_buf.index);
    }
  }

  if (!(pfd.revents & POLLIN)) {
    return false;
  }

  // Dequeue CAPTURE buffer (decoded frame)
  v4l2_plane plane = {};
  v4l2_buffer v4l_buf = {
    .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
    .memory = V4L2_MEMORY_USERPTR,
    .m = { .planes = &plane, },
    .length = 1,
  };
  int ret = try_ioctl(fd, VIDIOC_DQBUF, &v4l_buf);
  if (ret != 0) {
    return false;
  }

  if (env_debug_decoder) {
    printf("[V4LDecoder] getFrame: CAPTURE idx=%d bytesused=%d flags=0x%x\n",
           v4l_buf.index, v4l_buf.m.planes[0].bytesused, v4l_buf.flags);
  }

  // Sync the buffer from device
  buf_out[v4l_buf.index].sync(VISIONBUF_SYNC_FROM_DEVICE);

  // Copy NV12 data to output buffer with stride adjustment
  uint8_t *src = (uint8_t *)buf_out[v4l_buf.index].addr;
  int src_stride = decoded_stride;
  int dst_stride = (int)out_buf->stride;

  if (dst_stride == 0) dst_stride = width;

  // Copy Y plane
  for (int i = 0; i < height; i++) {
    memcpy(out_buf->y + i * dst_stride, src + i * src_stride, width);
  }

  // Copy UV plane (NV12: interleaved U/V, half height)
  int uv_src_offset = src_stride * VENUS_Y_SCANLINES(COLOR_FMT_NV12, height);
  for (int i = 0; i < height / 2; i++) {
    memcpy(out_buf->uv + i * dst_stride, src + uv_src_offset + i * src_stride, width);
  }

  // Re-queue the CAPTURE buffer
  queueCaptureBuffer(v4l_buf.index);

  return true;
}

void V4LDecoder::flush() {
  if (!is_open) return;

  // Flush OUTPUT queue
  struct v4l2_decoder_cmd cmd = {
    .cmd = V4L2_DEC_CMD_STOP,
    .flags = 0,
  };
  // Try to send stop command, but don't fail if not supported
  util::safe_ioctl(fd, VIDIOC_DECODER_CMD, &cmd);

  // Drain remaining CAPTURE buffers
  struct pollfd pfd = {.fd = fd, .events = POLLIN | POLLOUT, .revents = 0};
  while (true) {
    int rc = poll(&pfd, 1, 100);
    if (rc <= 0) break;

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

  // Now restart: send START command
  cmd.cmd = V4L2_DEC_CMD_START;
  util::safe_ioctl(fd, VIDIOC_DECODER_CMD, &cmd);
}

void V4LDecoder::close() {
  if (!is_open) return;

  // Stop streaming
  v4l2_buf_type buf_type;
  if (capture_ready) {
    buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    checked_ioctl(fd, VIDIOC_STREAMOFF, &buf_type);
  }
  buf_type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  checked_ioctl(fd, VIDIOC_STREAMOFF, &buf_type);

  // Release buffers
  request_buffers(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, 0);
  if (capture_ready) {
    request_buffers(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, 0);
  }

  // Free ION buffers
  for (int i = 0; i < V4L_DEC_BUF_IN_COUNT; i++) {
    buf_in[i].free();
  }
  if (capture_ready) {
    for (int i = 0; i < V4L_DEC_BUF_OUT_COUNT; i++) {
      buf_out[i].free();
    }
  }

  ::close(fd);
  fd = -1;
  is_open = false;
  capture_ready = false;
  fprintf(stderr, "[V4LDecoder] Closed\n");
}

V4LDecoder::V4LDecoder() {}

V4LDecoder::~V4LDecoder() {
  close();
}

#endif // QCOM2