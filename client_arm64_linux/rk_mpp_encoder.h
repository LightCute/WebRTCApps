// rk_mpp_encoder.h — Rockchip MPP H264 hardware encoder adapter
#ifndef APPS_PEERCONNECTION_CLIENT_RK_MPP_ENCODER_H_
#define APPS_PEERCONNECTION_CLIENT_RK_MPP_ENCODER_H_

#include <memory>
#include <vector>

#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder.h"
#include "api/environment/environment.h"

namespace webrtc {

// Factory template adapter — registered in VideoEncoderFactoryTemplate
struct MppH264EncoderTemplateAdapter {
  static std::vector<SdpVideoFormat> SupportedFormats();
  static std::unique_ptr<VideoEncoder> CreateEncoder(
      const Environment& env, const SdpVideoFormat& format);
  static bool IsScalabilityModeSupported(ScalabilityMode) { return false; }
};

// Actual MPP encoder implementation
class MppH264Encoder : public VideoEncoder {
 public:
  MppH264Encoder() = default;
  ~MppH264Encoder() override;

  int32_t InitEncode(const VideoCodec* codec_settings,
                     const Settings& settings) override;
  int32_t Encode(const VideoFrame& frame,
                 const std::vector<VideoFrameType>* frame_types) override;
  int32_t RegisterEncodeCompleteCallback(
      EncodedImageCallback* callback) override;
  int32_t Release() override;
  void SetRates(const RateControlParameters& parameters) override;
  EncoderInfo GetEncoderInfo() const override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace webrtc

#endif  // APPS_PEERCONNECTION_CLIENT_RK_MPP_ENCODER_H_
