#ifndef APPS_PEERCONNECTION_CLIENT_RGA_DECODED_SINK_H_
#define APPS_PEERCONNECTION_CLIENT_RGA_DECODED_SINK_H_

#include "api/video_codecs/video_decoder.h"
#include "api/video/video_frame.h"

namespace webrtc {
class MppH264Decoder;
}

class DmaBufPool;
struct ShmCtrlBlock;

class RgaDecodedSink : public webrtc::DecodedImageCallback {
 public:
  RgaDecodedSink();
  ~RgaDecodedSink() override;

  bool Init();
  void SetOutput(DmaBufPool* pool, ShmCtrlBlock* ctrl);
  void SetDecoder(webrtc::MppH264Decoder* decoder);

  int32_t Decoded(webrtc::VideoFrame& frame) override;

 private:
  webrtc::MppH264Decoder* decoder_ = nullptr;
  bool rga_loaded_ = false;
  void* rga_lib_ = nullptr;
  int (*rga_blit_)(void*, void*, void*) = nullptr;

  DmaBufPool* pool_ = nullptr;
  ShmCtrlBlock* ctrl_ = nullptr;
};

#endif
