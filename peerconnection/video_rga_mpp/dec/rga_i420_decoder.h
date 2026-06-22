#ifndef APPS_PEERCONNECTION_VIDEO_RGA_MPP_DEC_RGA_I420_DECODER_H_
#define APPS_PEERCONNECTION_VIDEO_RGA_MPP_DEC_RGA_I420_DECODER_H_

#include <memory>
#include <vector>

#include "api/video_codecs/video_decoder.h"
#include "api/video/video_frame.h"

class DmaBufPool;
struct ShmCtrlBlock;

namespace webrtc {
class Environment;
}

// Receives decoded NV12 frames from MPP decoder, RGA-converts to I420,
// writes to I420 DmaBufPool for consumer.
class RgaI420Decoder : public webrtc::DecodedImageCallback {
 public:
  RgaI420Decoder();
  ~RgaI420Decoder() override;

  bool Init();
  void SetOutput(DmaBufPool* i420_pool, ShmCtrlBlock* ctrl);

  // DecodedImageCallback
  int32_t Decoded(webrtc::VideoFrame& frame) override;

 private:
  bool loaded_ = false;
  void* rga_lib_ = nullptr;
  int (*rga_blit_)(void*, void*, void*) = nullptr;

  DmaBufPool* i420_pool_ = nullptr;
  ShmCtrlBlock* ctrl_ = nullptr;
};

#endif
