#include "apps/peerconnection/client/media_pipeline.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunsafe-buffer-usage"
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"

#include <memory>
#include <utility>

#include "api/audio/create_audio_device_module.h"
#include "api/video/video_sink_interface.h"
#include "apps/peerconnection/client/shm_common.h"
#include "rtc_base/logging.h"

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
