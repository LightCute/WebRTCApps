#include "media_source.h"

#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/enable_media.h"
#include "api/test/create_frame_generator.h"
#include "api/video/video_frame.h"
#include "api/video_codecs/video_decoder_factory_template.h"
#include "api/video_codecs/video_decoder_factory_template_libvpx_vp8_adapter.h"
#include "api/video_codecs/video_encoder_factory_template.h"
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp8_adapter.h"
#include "pc/video_track_source.h"
#include "rtc_base/logging.h"
#include "system_wrappers/include/clock.h"
#include "test/frame_generator_capturer.h"

namespace webrtc_media {

namespace {

// 合成视频源：移动方块测试图（第一阶段，无摄像头也能跑）。
class SyntheticVideoSource : public webrtc::VideoTrackSource {
 public:
  static webrtc::scoped_refptr<SyntheticVideoSource> Create(
      webrtc::TaskQueueFactory& task_queue_factory) {
    const int kWidth = 640;
    const int kHeight = 480;
    const int kFps = 30;
    auto frame_generator = webrtc::test::CreateSquareFrameGenerator(
        kWidth, kHeight, std::nullopt, std::nullopt);
    auto capturer = std::make_unique<webrtc::test::FrameGeneratorCapturer>(
        webrtc::Clock::GetRealTimeClock(), std::move(frame_generator), kFps,
        task_queue_factory);
    capturer->Start();
    return webrtc::make_ref_counted<SyntheticVideoSource>(std::move(capturer));
  }

 protected:
  explicit SyntheticVideoSource(
      std::unique_ptr<webrtc::test::FrameGeneratorCapturer> capturer)
      : webrtc::VideoTrackSource(/*remote=*/false),
        capturer_(std::move(capturer)) {}
  ~SyntheticVideoSource() override = default;

 private:
  webrtc::VideoSourceInterface<webrtc::VideoFrame>* source() override {
    return capturer_.get();
  }
  std::unique_ptr<webrtc::test::FrameGeneratorCapturer> capturer_;
};

// 远端视频帧计数 sink（第一阶段：只计数 + 打日志，不渲染）。
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

webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface>
CreateSyntheticVideoSource(webrtc::TaskQueueFactory& task_queue_factory) {
  return SyntheticVideoSource::Create(task_queue_factory);
}

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
