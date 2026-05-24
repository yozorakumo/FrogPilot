#pragma once

#ifdef QCOM2

#include <queue>
#include <mutex>
#include "msgq/visionipc/visionbuf.h"
#include "common/queue.h"

#define V4L_DEC_BUF_IN_COUNT 8   // OUTPUT (compressed HEVC input)
#define V4L_DEC_BUF_OUT_COUNT 6  // CAPTURE (decoded NV12 output)

// V4L2 hardware decoder for Qualcomm Venus (msm_vidc_vdec)
// Uses the same ION USERPTR pattern as V4LEncoder
class V4LDecoder {
public:
  V4LDecoder();
  ~V4LDecoder();

  bool open(int width, int height);

  // Feed compressed HEVC data (non-blocking, drains completed CAPTURE buffers)
  bool feed(const uint8_t *data, size_t size);

  // Get decoded NV12 frame (blocking with timeout)
  bool getFrame(VisionBuf *out_buf);

  // Flush decoder state (for seeking)
  void flush();

  void close();

  int width = 0, height = 0;

private:
  int fd = -1;
  bool is_open = false;

  VisionBuf buf_in[V4L_DEC_BUF_IN_COUNT];   // OUTPUT (compressed HEVC)
  VisionBuf buf_out[V4L_DEC_BUF_OUT_COUNT]; // CAPTURE (decoded NV12)

  SafeQueue<int> free_input_bufs;

  size_t input_buf_size = 0;
  size_t output_buf_size = 0;
  int decoded_stride = 0;

  void drainCapture();
  void queueOutputBuffer(int index, uint32_t bytesused);
  void queueCaptureBuffer(int index);
};

#endif // QCOM2