// rga_video_converter.h — RGA hardware YUYV→I420 via dma-buf
#ifndef APPS_PEERCONNECTION_VIDEO_CAPTURE_SHM_RGA_RGA_VIDEO_CONVERTER_H_
#define APPS_PEERCONNECTION_VIDEO_CAPTURE_SHM_RGA_RGA_VIDEO_CONVERTER_H_

#include <cstddef>
#include <cstdint>

#include "modules/video_capture/raw_video_sink_interface.h"
#include "modules/video_capture/video_capture_defines.h"

class DmaBufPool;
struct ShmCtrlBlock;

// Receives raw YUYV frames from VCM, converts to I420 via RGA hardware
// writing directly to a dma-buf pool slot (zero CPU copy).
// Frame metadata is written to the shared control block for consumer sync.
class RgaVideoConverter : public webrtc::RawVideoSinkInterface {
 public:
  RgaVideoConverter();
  ~RgaVideoConverter() override;

  // Load librga, init RGA. Returns true if RGA hardware path is ready.
  bool Init();

  // Set the output target: dma-buf pool + control block for ring buffer sync.
  void SetOutput(DmaBufPool* pool, ShmCtrlBlock* ctrl);

  // RawVideoSinkInterface — called from VCM capture thread
  int32_t OnRawFrame(uint8_t* videoFrame,
                     size_t videoFrameLength,
                     const webrtc::VideoCaptureCapability& frameInfo,
                     webrtc::VideoRotation rotation,
                     int64_t captureTime) override;

 private:
  bool loaded_ = false;

  // dlopen handles
  void* lib_handle_ = nullptr;
  int  (*fp_rga_init_)();
  void (*fp_rga_deinit_)();
  int  (*fp_rga_blit_)(void* src, void* dst, void* src1);

  // DMA-BUF output
  DmaBufPool* pool_ = nullptr;
  ShmCtrlBlock* ctrl_ = nullptr;
};

#endif  // APPS_PEERCONNECTION_VIDEO_CAPTURE_SHM_RGA_RGA_VIDEO_CONVERTER_H_
