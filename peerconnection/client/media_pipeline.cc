#include "apps/peerconnection/client/media_pipeline.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunsafe-buffer-usage"
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"

#include <memory>
#include <string>
#include <utility>

#include "absl/flags/declare.h"
#include "absl/flags/flag.h"

ABSL_DECLARE_FLAG(std::string, audio_source);

#include "api/audio/audio_device_defines.h"
#include "api/audio/create_audio_device_module.h"
#include "api/audio_options.h"
#include "api/make_ref_counted.h"
#include "api/media_stream_interface.h"
#include "api/peer_connection_interface.h"
#include "api/rtc_error.h"
#include "api/scoped_refptr.h"
#include "api/task_queue/task_queue_factory.h"
#include "api/test/create_frame_generator.h"
#include "api/video/video_source_interface.h"
#include "apps/peerconnection/client/defaults.h"
#include "apps/peerconnection/client/shm_audio_capturer.h"
#include "apps/peerconnection/client/shm_common.h"
#include "json/json.h"
#include "modules/video_capture/video_capture.h"
#include "modules/video_capture/video_capture_factory.h"
#include "pc/video_track_source.h"
#include "rtc_base/checks.h"
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

// ==================== MediaPipeline ====================

MediaPipeline::MediaPipeline(const webrtc::Environment& env,
                             webrtc::Thread* worker_thread)
    : env_(env), worker_thread_(worker_thread) {}

MediaPipeline::~MediaPipeline() = default;

void MediaPipeline::SetEventCallback(EventCallback cb) {
  event_cb_ = std::move(cb);
}

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

webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface>
MediaPipeline::CreateVideoSource() {
  video_source_ = CapturerTrackSource::Create(env_.task_queue_factory());
  return video_source_;
}

// ---- Renderers ----

void MediaPipeline::StartLocalRenderer(webrtc::VideoTrackInterface* track) {
  if (local_renderer_) { RTC_LOG(LS_WARNING) << "Local renderer already started"; return; }
  local_renderer_ = std::make_unique<ShmVideoRenderer>(
      shm_key_path() + "_local", SHM_PROJ_ID + 1);
  track->AddOrUpdateSink(local_renderer_.get(), webrtc::VideoSinkWants());
  RTC_LOG(LS_INFO) << "Local SHM renderer started";
}

void MediaPipeline::StopLocalRenderer() {
  local_renderer_.reset();
}

void MediaPipeline::StartRemoteRenderer(webrtc::VideoTrackInterface* track) {
  if (remote_renderer_) { RTC_LOG(LS_WARNING) << "Remote renderer already started"; return; }
  remote_renderer_ = std::make_unique<ShmVideoRenderer>(
      shm_key_path() + "_remote", SHM_PROJ_ID + 2);
  track->AddOrUpdateSink(remote_renderer_.get(), webrtc::VideoSinkWants());
  RTC_LOG(LS_INFO) << "Remote SHM renderer started";
}

void MediaPipeline::StopRemoteRenderer() {
  remote_renderer_.reset();
}

void MediaPipeline::StartRemoteAudioRenderer(webrtc::AudioTrackInterface* track) {
  if (remote_audio_renderer_) { RTC_LOG(LS_WARNING) << "Remote audio renderer already started"; return; }
  remote_audio_renderer_ = std::make_unique<ShmAudioRenderer>(
      shm_audio_playout_key_path(), SHM_AUDIO_PLAYOUT_PROJ_ID);
  track->AddSink(remote_audio_renderer_.get());
  RTC_LOG(LS_INFO) << "Remote audio SHM renderer started";
}

void MediaPipeline::StopRemoteAudioRenderer() {
  remote_audio_renderer_.reset();
}

// ---- Device management ----

void MediaPipeline::QueryDevices() {
  Json::Value video_arr(Json::arrayValue);
  auto info = webrtc::VideoCaptureFactory::CreateDeviceInfo();
  if (info) {
    int n = info->NumberOfDevices();
    char name[256], id[256];
    for (int i = 0; i < n; ++i) {
      if (info->GetDeviceName(i, name, sizeof(name), id, sizeof(id)) == 0) {
        Json::Value dev;
        dev["idx"] = i; dev["name"] = name;
        video_arr.append(dev);
      }
    }
  }
  Json::StreamWriterBuilder factory;
  factory["indentation"] = "";
  Json::Value video_event;
  video_event["event"] = "video_devices";
  video_event["devices"] = video_arr;
  if (event_cb_) event_cb_(Json::writeString(factory, video_event));

  Json::Value audio_arr(Json::arrayValue);
  if (adm_ && worker_thread_) {
    audio_arr = worker_thread_->BlockingCall([this]() -> Json::Value {
      Json::Value arr(Json::arrayValue);
      int16_t n = adm_->RecordingDevices();
      char name[webrtc::kAdmMaxDeviceNameSize];
      char guid[webrtc::kAdmMaxGuidSize];
      for (int16_t i = 0; i < n; ++i) {
        if (adm_->RecordingDeviceName(i, name, guid) == 0) {
          Json::Value dev;
          dev["idx"] = i; dev["name"] = name;
          arr.append(dev);
        }
      }
      return arr;
    });
  }
  if (!audio_arr.empty()) {
    Json::Value audio_event;
    audio_event["event"] = "audio_input_devices";
    audio_event["devices"] = audio_arr;
    if (event_cb_) event_cb_(Json::writeString(factory, audio_event));
  }

  if (audio_arr.empty()) {
    FILE* fp = popen("arecord -l 2>/dev/null", "r");
    if (fp) {
      Json::Value alsa_arr(Json::arrayValue);
      char line[256]; int idx = 0;
      while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "card ") == line && strstr(line, "device ")) {
          char* desc_begin = strrchr(line, '[');
          char* desc_end = desc_begin ? strrchr(line, ']') : nullptr;
          char name[256];
          if (desc_begin && desc_end && desc_end > desc_begin) {
            size_t len = desc_end - desc_begin - 1;
            snprintf(name, sizeof(name), "%.*s", (int)len, desc_begin + 1);
          } else {
            snprintf(name, sizeof(name), "Capture device %d", idx);
          }
          Json::Value dev;
          dev["idx"] = idx; dev["name"] = name;
          alsa_arr.append(dev); idx++;
        }
      }
      pclose(fp);
      if (!alsa_arr.empty()) {
        Json::Value alsa_event;
        alsa_event["event"] = "audio_input_devices";
        alsa_event["devices"] = alsa_arr;
        if (event_cb_) event_cb_(Json::writeString(factory, alsa_event));
      }
    }
  }
}

void MediaPipeline::SetVideoDevice(int device_idx) {
  if (!video_source_) { RTC_LOG(LS_WARNING) << "No local video source to swap"; return; }
  auto new_capturer = CreateCapturer(env_.task_queue_factory(), device_idx);
  if (!new_capturer) {
    RTC_LOG(LS_ERROR) << "Failed to create capturer for device " << device_idx;
    return;
  }
  auto* capturer_source =
      static_cast<CapturerTrackSource*>(video_source_.get());
  capturer_source->SwapCapturer(std::move(new_capturer));
  video_device_idx_ = device_idx;
}

void MediaPipeline::SetAudioInputDevice(int device_idx) {
  audio_input_device_idx_ = device_idx;
  if (!adm_) return;
  worker_thread_->BlockingCall([this, device_idx] {
    bool was_recording = adm_->Recording();
    if (was_recording) {
      adm_->StopRecording();
      adm_->SetRecordingDevice(device_idx);
      adm_->InitRecording();
      adm_->StartRecording();
    } else {
      adm_->SetRecordingDevice(device_idx);
    }
  });
}

// ---- Media control ----

void MediaPipeline::SetAudioMuted(bool muted) {
  RTC_LOG(LS_INFO) << "SetAudioMuted: " << (muted ? "true" : "false");
  if (adm_ && muted) adm_->StopRecording();
}

void MediaPipeline::SetVideoPaused(bool paused, webrtc::PeerConnectionInterface* pc) {
  RTC_LOG(LS_INFO) << "SetVideoPaused: " << (paused ? "true" : "false");
  if (!pc) return;
  auto senders = pc->GetSenders();
  for (auto& sender : senders) {
    if (sender->track() &&
        sender->track()->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
      sender->track()->set_enabled(!paused);
    }
  }
}

// ---- Lifecycle ----

void MediaPipeline::Shutdown() {
  local_renderer_.reset();
  remote_renderer_.reset();
  remote_audio_renderer_.reset();
  if (adm_ && worker_thread_) {
    auto adm = std::move(adm_);
    adm_ = nullptr;
    worker_thread_->BlockingCall([adm = std::move(adm)]() mutable {
      if (adm->Playing()) adm->StopPlayout();
      if (adm->Recording()) adm->StopRecording();
      adm = nullptr;
    });
  }
  video_source_ = nullptr;
  audio_source_ = nullptr;
}

#pragma GCC diagnostic pop
