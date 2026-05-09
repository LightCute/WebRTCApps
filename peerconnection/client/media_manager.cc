#include "apps/peerconnection/client/media_manager.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunsafe-buffer-usage"
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>

#include "absl/memory/memory.h"
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
      if (capturer) {
        return capturer;
      }
    }
    for (int i = 0; i < num_devices; ++i) {
      std::unique_ptr<TestVideoCapturer> capturer =
          webrtc::test::CreateVideoCapturer(kWidth, kHeight, kFps, i);
      if (capturer) {
        return capturer;
      }
    }
  }
  RTC_LOG(LS_WARNING)
      << "No video capture device found; using synthetic video.";
  auto frame_generator = webrtc::test::CreateSquareFrameGenerator(
      kWidth, kHeight, std::nullopt, std::nullopt);
  return std::make_unique<webrtc::test::FrameGeneratorCapturer>(
      webrtc::Clock::GetRealTimeClock(), std::move(frame_generator), kFps,
      task_queue_factory);
}

class CapturerTrackSource : public webrtc::VideoTrackSource {
 public:
  static webrtc::scoped_refptr<CapturerTrackSource> Create(
      webrtc::TaskQueueFactory& task_queue_factory,
      int device_idx = -1) {
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

// ==================== MediaManager ====================

MediaManager::MediaManager(const webrtc::Environment& env,
                           webrtc::Thread* worker_thread)
    : env_(env),
      worker_thread_(worker_thread) {}

MediaManager::~MediaManager() = default;

// ---- ADM lifecycle ----

bool MediaManager::CreateAudioDeviceModule() {
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

webrtc::AudioDeviceModule* MediaManager::GetADM() const {
  return adm_.get();
}

// ---- Track creation ----

bool MediaManager::AddTracks(
    webrtc::PeerConnectionFactoryInterface* factory,
    webrtc::PeerConnectionInterface* pc,
    webrtc::AudioSourceInterface* external_audio_source) {
  if (!pc->GetSenders().empty()) {
    return true;  // Already added tracks.
  }

  // Audio track: use external source if provided, otherwise create from ADM
  if (external_audio_source) {
    audio_source_ = external_audio_source;
  } else {
    audio_source_ = factory->CreateAudioSource(webrtc::AudioOptions());
  }

  webrtc::scoped_refptr<webrtc::AudioTrackInterface> audio_track(
      factory->CreateAudioTrack(kAudioLabel, audio_source_.get()));
  auto result_or_error = pc->AddTrack(audio_track, {kStreamId});
  if (!result_or_error.ok()) {
    RTC_LOG(LS_ERROR) << "Failed to add audio track to PeerConnection: "
                      << result_or_error.error().message();
  }

  video_source_ = CapturerTrackSource::Create(env_.task_queue_factory());
  if (video_source_) {
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> video_track(
        factory->CreateVideoTrack(video_source_, kVideoLabel));
    // Note: local SHM renderer is wired externally by the engine

    result_or_error = pc->AddTrack(video_track, {kStreamId});
    if (!result_or_error.ok()) {
      RTC_LOG(LS_ERROR) << "Failed to add video track to PeerConnection: "
                        << result_or_error.error().message();
    }
  } else {
    RTC_LOG(LS_WARNING)
        << "No local video track; proceeding without local video";
  }

  if (event_cb_)
    event_cb_(R"({"event":"call_connected"})");

  return true;
}

// ---- Device enumeration ----

void MediaManager::QueryDevices() {
  Json::Value video_arr(Json::arrayValue);
  auto info = webrtc::VideoCaptureFactory::CreateDeviceInfo();
  if (info) {
    int n = info->NumberOfDevices();
    char name[256];
    char id[256];
    for (int i = 0; i < n; ++i) {
      if (info->GetDeviceName(i, name, sizeof(name), id, sizeof(id)) == 0) {
        Json::Value dev;
        dev["idx"] = i;
        dev["name"] = name;
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
          dev["idx"] = i;
          dev["name"] = name;
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

  // ALSA fallback
  if (audio_arr.empty()) {
    FILE* fp = popen("arecord -l 2>/dev/null", "r");
    if (fp) {
      Json::Value alsa_arr(Json::arrayValue);
      char line[256];
      int idx = 0;
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
          dev["idx"] = idx;
          dev["name"] = name;
          alsa_arr.append(dev);
          idx++;
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

// ---- Device selection ----

void MediaManager::SetVideoDevice(int device_idx) {
  if (!video_source_) {
    RTC_LOG(LS_WARNING) << "No local video source to swap";
    return;
  }
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

void MediaManager::SetAudioInputDevice(int device_idx) {
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

void MediaManager::SetAudioMuted(bool muted) {
  RTC_LOG(LS_INFO) << "SetAudioMuted: " << (muted ? "true" : "false");
  if (adm_) {
    if (muted) {
      adm_->StopRecording();
    }
  }
}

void MediaManager::SetVideoPaused(bool paused,
                                   webrtc::PeerConnectionInterface* pc) {
  RTC_LOG(LS_INFO) << "SetVideoPaused: " << (paused ? "true" : "false");
  if (pc) {
    auto senders = pc->GetSenders();
    for (auto& sender : senders) {
      if (sender->track() &&
          sender->track()->kind() ==
              webrtc::MediaStreamTrackInterface::kVideoKind) {
        sender->track()->set_enabled(!paused);
      }
    }
  }
}

// ---- Events ----

void MediaManager::SetEventCallback(EventCallback cb) {
  event_cb_ = std::move(cb);
}

// ---- Lifecycle ----

void MediaManager::Shutdown() {
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
