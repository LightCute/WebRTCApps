#include "media/video_codec.h"

#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/enable_media.h"
#include "api/video/video_frame.h"
#include "api/video_codecs/video_decoder_factory_template.h"
#include "api/video_codecs/video_decoder_factory_template_libvpx_vp8_adapter.h"
#include "api/video_codecs/video_encoder_factory_template.h"
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp8_adapter.h"
#include "rtc_base/logging.h"

namespace webrtc_media {

namespace {

// 远端视频帧计数 sink（只计数 + 打日志，不渲染）。
class RemoteVideoFrameCounter
    : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
 public:
  void OnFrame(const webrtc::VideoFrame& frame) override {
    count_++;
    if (count_ == 1 || count_ % 30 == 0) {
      RTC_LOG(LS_INFO) << "Remote video frame #" << count_ << " "
                       << frame.width() << "x" << frame.height()
                       << " ntp_ms=" << frame.ntp_time_ms();
    }
  }

 private:
  int count_ = 0;
};

}  // namespace

std::unique_ptr<webrtc::VideoSinkInterface<webrtc::VideoFrame>>
CreateRemoteVideoFrameCounter() {
  return std::make_unique<RemoteVideoFrameCounter>();
}

void ConfigureVideoCodecs(webrtc::PeerConnectionFactoryDependencies& deps) {
  deps.audio_encoder_factory = webrtc::CreateBuiltinAudioEncoderFactory();
  deps.audio_decoder_factory = webrtc::CreateBuiltinAudioDecoderFactory();
  deps.video_encoder_factory =
      std::make_unique<webrtc::VideoEncoderFactoryTemplate<
          webrtc::LibvpxVp8EncoderTemplateAdapter>>();
  deps.video_decoder_factory =
      std::make_unique<webrtc::VideoDecoderFactoryTemplate<
          webrtc::LibvpxVp8DecoderTemplateAdapter>>();
  webrtc::EnableMedia(deps);
}

}  // namespace webrtc_media
