#include "media/audio_processing.h"

#include "api/audio/builtin_audio_processing_builder.h"
#include "api/environment/environment.h"
#include "rtc_base/logging.h"

namespace webrtc_media {

namespace {

// 包一层内置 builder，把构建出来的 APM 存下来，供运行时 ApplyConfig 使用。
class CapturingApmBuilder : public webrtc::AudioProcessingBuilderInterface {
 public:
  explicit CapturingApmBuilder(
      webrtc::scoped_refptr<webrtc::AudioProcessing>* out_apm)
      : out_apm_(out_apm) {}

  webrtc::scoped_refptr<webrtc::AudioProcessing> Build(
      const webrtc::Environment& env) override {
    webrtc::scoped_refptr<webrtc::AudioProcessing> apm = builder_.Build(env);
    if (apm && out_apm_ != nullptr) {
      *out_apm_ = apm;
    }
    return apm;
  }

 private:
  webrtc::BuiltinAudioProcessingBuilder builder_;
  webrtc::scoped_refptr<webrtc::AudioProcessing>* out_apm_;
};

}  // namespace

void ConfigureAudioProcessing(
    webrtc::PeerConnectionFactoryDependencies& deps,
    webrtc::scoped_refptr<webrtc::AudioProcessing>* out_apm) {
  deps.audio_processing_builder =
      std::make_unique<CapturingApmBuilder>(out_apm);
}

bool ApplyAudioProcessing(webrtc::AudioProcessing* apm,
                          const AudioProcessingState& state) {
  if (apm == nullptr) {
    return false;
  }
  // 与 WebRtcVoiceEngine::ApplyOptions 同口径：在现有配置上只改这三个开关。
  webrtc::AudioProcessing::Config config = apm->GetConfig();
  config.echo_canceller.enabled = state.aec;
  config.noise_suppression.enabled = state.ns;
  config.noise_suppression.level =
      webrtc::AudioProcessing::Config::NoiseSuppression::Level::kHigh;
  config.gain_controller1.enabled = state.agc;
  config.gain_controller1.mode =
      webrtc::AudioProcessing::Config::GainController1::kAdaptiveAnalog;
  apm->ApplyConfig(config);
  RTC_LOG(LS_INFO) << "ApplyAudioProcessing aec=" << state.aec
                   << " ns=" << state.ns << " agc=" << state.agc;
  return true;
}

}  // namespace webrtc_media
