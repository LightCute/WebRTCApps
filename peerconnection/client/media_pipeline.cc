#include "apps/peerconnection/client/media_pipeline.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunsafe-buffer-usage"
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"

#include <memory>
#include <mutex>
#include <utility>

#include "api/audio/create_audio_device_module.h"
#include "api/audio_options.h"
#include "api/make_ref_counted.h"
#include "api/task_queue/task_queue_factory.h"
#include "api/test/create_frame_generator.h"
#include "api/video/video_sink_interface.h"
#include "api/video/video_source_interface.h"
#include "apps/peerconnection/client/shm_common.h"
#include "modules/video_capture/video_capture.h"
#include "modules/video_capture/video_capture_factory.h"
#include "pc/video_track_source.h"
#include "rtc_base/logging.h"
#include "system_wrappers/include/clock.h"
#include "test/frame_generator_capturer.h"
#include "test/platform_video_capturer.h"
#include "test/test_video_capturer.h"

namespace {

using webrtc::test::TestVideoCapturer;

std::unique_ptr<TestVideoCapturer> CreateCapturer(
    webrtc::TaskQueueFactory& task_queue_factory,
    int device_idx = -1) {
  const size_t kWidth = 640;
  const size_t kHeight = 480;
  const size_t kFps = 30;
  std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> info(
      webrtc::VideoCaptureFactory::CreateDeviceInfo());
  if (info) {
    int num_devices = info->NumberOfDevices();
    if (device_idx >= 0 && device_idx < num_devices) {
      std::unique_ptr<TestVideoCapturer> capturer =
          webrtc::test::CreateVideoCapturer(kWidth, kHeight, kFps, device_idx);
      if (capturer) return capturer;
    }
    for (int i = 0; i < num_devices; ++i) {
      std::unique_ptr<TestVideoCapturer> capturer =
          webrtc::test::CreateVideoCapturer(kWidth, kHeight, kFps, i);
      if (capturer) return capturer;
    }
  }
  RTC_LOG(LS_WARNING) << "No video capture device found; using synthetic video.";
  auto frame_generator = webrtc::test::CreateSquareFrameGenerator(
      kWidth, kHeight, std::nullopt, std::nullopt);
  return std::make_unique<webrtc::test::FrameGeneratorCapturer>(
      webrtc::Clock::GetRealTimeClock(), std::move(frame_generator), kFps,
      task_queue_factory);
}

class CapturerTrackSource : public webrtc::VideoTrackSource {
 public:
  static webrtc::scoped_refptr<CapturerTrackSource> Create(
      webrtc::TaskQueueFactory& task_queue_factory, int device_idx = -1) {
    std::unique_ptr<TestVideoCapturer> capturer =
        CreateCapturer(task_queue_factory, device_idx);
    if (capturer) {
      capturer->Start();
      return webrtc::make_ref_counted<CapturerTrackSource>(std::move(capturer));
    }
    return nullptr;
  }

  void SwapCapturer(std::unique_ptr<TestVideoCapturer> new_capturer) {
    if (!new_capturer) return;
    RTC_LOG(LS_INFO) << "Swapping video capturer";
    {
      std::lock_guard<std::mutex> lock(capturer_mutex_);
      capturer_->Stop();
      capturer_ = std::move(new_capturer);
      capturer_->Start();
    }
  }

 protected:
  explicit CapturerTrackSource(std::unique_ptr<TestVideoCapturer> capturer)
      : VideoTrackSource(/*remote=*/false), capturer_(std::move(capturer)) {}
  ~CapturerTrackSource() override = default;

 private:
  webrtc::VideoSourceInterface<webrtc::VideoFrame>* source() override {
    return capturer_.get();
  }
  std::mutex capturer_mutex_;
  std::unique_ptr<TestVideoCapturer> capturer_;
};

}  // namespace

MediaPipeline::MediaPipeline(const webrtc::Environment& env,
                             webrtc::Thread* worker_thread)
    : env_(env), worker_thread_(worker_thread) {}

MediaPipeline::~MediaPipeline() = default;

// ---- ADM ----

bool MediaPipeline::CreateAudioDeviceModule() {
  adm_ = worker_thread_->BlockingCall([this] {
    return webrtc::CreateAudioDeviceModule(
        env_, webrtc::AudioDeviceModule::kPlatformDefaultAudio);
  });
  if (!adm_) {
    RTC_LOG(LS_ERROR) << "Failed to create AudioDeviceModule";
    return false;
  }
  return true;
}

// ---- Sources ----

webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface>
MediaPipeline::CreateVideoSource() {
  video_source_ = CapturerTrackSource::Create(env_.task_queue_factory());
  return video_source_;
}

webrtc::AudioSourceInterface* MediaPipeline::CreateAudioSource(
    webrtc::PeerConnectionFactoryInterface* factory,
    webrtc::AudioSourceInterface* external_source) {
  if (external_source) {
    audio_source_ = external_source;
  } else {
    audio_source_ = factory->CreateAudioSource(webrtc::AudioOptions());
  }
  return audio_source_.get();
}

// ---- Device management ----

void MediaPipeline::SetVideoDevice(int device_idx) {
  if (!video_source_) {
    RTC_LOG(LS_WARNING) << "No local video source to swap";
    return;
  }
  auto new_capturer = CreateCapturer(env_.task_queue_factory(), device_idx);
  if (!new_capturer) {
    RTC_LOG(LS_ERROR) << "Failed to create capturer for device " << device_idx;
    return;
  }
  auto* capturer_source = static_cast<CapturerTrackSource*>(video_source_.get());
  capturer_source->SwapCapturer(std::move(new_capturer));
  video_device_idx_ = device_idx;
}

// ---- Renderers ----

void MediaPipeline::StartLocalRenderer(webrtc::VideoTrackInterface* track) {
  if (local_renderer_) {
    RTC_LOG(LS_WARNING) << "Local SHM renderer already started";
    return;
  }
  local_renderer_ = std::make_unique<ShmVideoRenderer>(
      shm_key_path() + "_local", SHM_PROJ_ID + 1);
  track->AddOrUpdateSink(local_renderer_.get(), webrtc::VideoSinkWants());
  RTC_LOG(LS_INFO) << "Local SHM renderer started";
}

void MediaPipeline::StopLocalRenderer() {
  if (local_renderer_) {
    local_renderer_.reset();
    RTC_LOG(LS_INFO) << "Local SHM renderer stopped";
  }
}

void MediaPipeline::StartRemoteRenderer(webrtc::VideoTrackInterface* track) {
  if (remote_renderer_) {
    RTC_LOG(LS_WARNING) << "Remote SHM renderer already started";
    return;
  }
  remote_renderer_ = std::make_unique<ShmVideoRenderer>(
      shm_key_path() + "_remote", SHM_PROJ_ID + 2);
  track->AddOrUpdateSink(remote_renderer_.get(), webrtc::VideoSinkWants());
  RTC_LOG(LS_INFO) << "Remote SHM renderer started";
}

void MediaPipeline::StopRemoteRenderer() {
  if (remote_renderer_) {
    remote_renderer_.reset();
    RTC_LOG(LS_INFO) << "Remote SHM renderer stopped";
  }
}

void MediaPipeline::StartRemoteAudioRenderer(webrtc::AudioTrackInterface* track) {
  if (remote_audio_renderer_) {
    RTC_LOG(LS_WARNING) << "Remote audio SHM renderer already started";
    return;
  }
  remote_audio_renderer_ = std::make_unique<ShmAudioRenderer>(
      shm_audio_playout_key_path(), SHM_AUDIO_PLAYOUT_PROJ_ID);
  track->AddSink(remote_audio_renderer_.get());
  RTC_LOG(LS_INFO) << "Remote audio SHM renderer started";
}

void MediaPipeline::StopRemoteAudioRenderer() {
  if (remote_audio_renderer_) {
    remote_audio_renderer_.reset();
    RTC_LOG(LS_INFO) << "Remote audio SHM renderer stopped";
  }
}

// ---- Lifecycle ----

void MediaPipeline::Shutdown() {
  local_renderer_.reset();
  remote_renderer_.reset();
  remote_audio_renderer_.reset();
  video_source_ = nullptr;
  audio_source_ = nullptr;
  if (adm_ && worker_thread_) {
    auto adm = std::move(adm_);
    adm_ = nullptr;
    worker_thread_->BlockingCall([adm = std::move(adm)]() mutable {
      if (adm->Playing()) adm->StopPlayout();
      if (adm->Recording()) adm->StopRecording();
      adm = nullptr;
    });
  }
}

#pragma GCC diagnostic pop
