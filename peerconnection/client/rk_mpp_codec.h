#ifndef APPS_PEERCONNECTION_CLIENT_RK_MPP_CODEC_H_
#define APPS_PEERCONNECTION_CLIENT_RK_MPP_CODEC_H_

#include <memory>
#include <vector>

#include "api/environment/environment.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_decoder.h"
#include "api/video_codecs/video_encoder.h"

namespace webrtc {

struct MppH264EncoderTemplateAdapter {
  static std::vector<SdpVideoFormat> SupportedFormats();
  static std::unique_ptr<VideoEncoder> CreateEncoder(
      const Environment& env, const SdpVideoFormat& format);
  static bool IsScalabilityModeSupported(ScalabilityMode) { return false; }
};

struct MppH264DecoderTemplateAdapter {
  static std::vector<SdpVideoFormat> SupportedFormats();
  static std::unique_ptr<VideoDecoder> CreateDecoder(
      const Environment& env, const SdpVideoFormat& format);
};

class MppH264Encoder : public VideoEncoder {
 public:
  MppH264Encoder();
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

class MppH264Decoder : public VideoDecoder {
 public:
  MppH264Decoder();
  ~MppH264Decoder() override;

  bool Configure(const Settings& settings) override;
  int32_t Decode(const EncodedImage& input_image,
                 int64_t render_time_ms) override;
  int32_t RegisterDecodeCompleteCallback(
      DecodedImageCallback* callback) override;
  int32_t Release() override;
  DecoderInfo GetDecoderInfo() const override;
  int GetLastNV12Fd() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace webrtc

#endif  // APPS_PEERCONNECTION_CLIENT_RK_MPP_CODEC_H_
