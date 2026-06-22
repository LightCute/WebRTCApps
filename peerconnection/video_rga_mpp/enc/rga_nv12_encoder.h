#ifndef APPS_PEERCONNECTION_VIDEO_RGA_MPP_ENC_RGA_NV12_ENCODER_H_
#define APPS_PEERCONNECTION_VIDEO_RGA_MPP_ENC_RGA_NV12_ENCODER_H_

#include <memory>
#include <vector>

#include "api/video/video_frame.h"
#include "api/video_codecs/video_encoder.h"
#include "modules/video_capture/raw_video_sink_interface.h"
#include "modules/video_capture/video_capture_defines.h"

class DmaBufPool;
struct ShmCtrlBlock;

namespace webrtc {
class Environment;
}

// Receives raw YUYV from VCM, RGA-converts to NV12, feeds MPP H.264 hardware
// encoder, writes H264 bitstream to DmaBufPool. Implements EncodedImageCallback
// to capture encoder output.
class RgaNv12Encoder : public webrtc::RawVideoSinkInterface,
                       public webrtc::EncodedImageCallback {
 public:
  RgaNv12Encoder();
  ~RgaNv12Encoder() override;

  bool Init(webrtc::Environment& env);
  void SetOutput(DmaBufPool* h264_pool, ShmCtrlBlock* ctrl);

  // RawVideoSinkInterface
  int32_t OnRawFrame(uint8_t* videoFrame, size_t videoFrameLength,
                     const webrtc::VideoCaptureCapability& frameInfo,
                     webrtc::VideoRotation rotation,
                     int64_t captureTime) override;

  // EncodedImageCallback
  Result OnEncodedImage(const webrtc::EncodedImage& encoded_image,
                        const webrtc::CodecSpecificInfo* codec_specific_info) override;

 private:
  bool InitEncoder(int width, int height);

  bool loaded_ = false;
  int enc_width_ = 0, enc_height_ = 0;

  void* rga_lib_ = nullptr;
  int (*rga_blit_)(void*, void*, void*) = nullptr;
  std::vector<uint8_t> nv12_buf_;

  std::unique_ptr<webrtc::VideoEncoder> encoder_;
  DmaBufPool* h264_pool_ = nullptr;
  ShmCtrlBlock* ctrl_ = nullptr;
};

#endif
