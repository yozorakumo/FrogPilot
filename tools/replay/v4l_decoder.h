#pragma once

#ifdef QCOM2

#include <queue>
#include <mutex>
#include <vector>
#include "msgq/visionipc/visionbuf.h"
#include "common/queue.h"

#define V4L_DEC_BUF_IN_COUNT 8   // OUTPUT (compressed HEVC input)
#define V4L_DEC_BUF_OUT_COUNT 6  // CAPTURE (decoded NV12 output)

// V4L2 hardware decoder for Qualcomm Venus (msm_vidc_vdec)
// Uses DMABUF for CAPTURE (required by SDM845 Venus driver) and USERPTR for OUTPUT.
//   1. Set OUTPUT format (HEVC compressed input)
//   2. REQBUFS + allocate USERPTR buffers for OUTPUT
//   3. STREAMON OUTPUT
//   4. Feed VPS/SPS/PPS with CODECCONFIG flag
//   5. Feed compressed frames, wait for SOURCE_CHANGE
//   6. Set CAPTURE format (NV12), REQBUFS with DMABUF
//   7. Allocate ION buffers, export fd, pass to V4L2 as DMABUF
//   8. STREAMON CAPTURE, queue empty DMABUF buffers
//   9. Dequeue decoded frames from CAPTURE
class V4LDecoder {
public:
  V4LDecoder();
  ~V4LDecoder();

  bool open(int width, int height);

  // Feed compressed HEVC data (drains completed OUTPUT buffers to free them)
  bool feed(const uint8_t *data, size_t size);

  // Feed codec config (VPS/SPS/PPS) with CODECCONFIG flag before first frame
  bool feedExtradata(const uint8_t *data, size_t size);

  // Get decoded NV12 frame (blocking with timeout)
  bool getFrame(VisionBuf *out_buf);

  // Flush decoder state (for seeking)
  void flush();

  void close();

  int width = 0, height = 0;

  // Drain CAPTURE/OUTPUT buffers (non-blocking, re-queues them)
  void drainCapture();
  void drainOutput();
  bool isCaptureReady() const { return capture_ready; }

private:
  int fd = -1;
  bool is_open = false;
  bool capture_ready = false;
  bool extradata_sent = false;

  VisionBuf buf_in[V4L_DEC_BUF_IN_COUNT];   // OUTPUT (compressed HEVC) - USERPTR
  VisionBuf buf_out[V4L_DEC_BUF_OUT_COUNT]; // CAPTURE (decoded NV12) - DMABUF

  SafeQueue<int> free_input_bufs;

  size_t input_buf_size = 0;
  size_t output_buf_size = 0;
  int decoded_stride = 0;

  // Extradata buffer for VPS/SPS/PPS (CODECCONFIG)
  std::vector<uint8_t> extradata;

  bool setupCapture();
  void queueOutputBuffer(int index, uint32_t bytesused, uint32_t flags = 0);
  void queueCaptureBuffer(int index);
};

#endif // QCOM2