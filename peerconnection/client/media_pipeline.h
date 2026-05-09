#ifndef APPS_PEERCONNECTION_CLIENT_MEDIA_PIPELINE_H_
#define APPS_PEERCONNECTION_CLIENT_MEDIA_PIPELINE_H_

#include <memory>

#include "api/audio/audio_device.h"
#include "api/environment/environment.h"
#include "api/media_stream_interface.h"
#include "api/scoped_refptr.h"
#include "apps/peerconnection/client/shm_audio_renderer.h"
#include "apps/peerconnection/client/shm_video_renderer.h"
#include "rtc_base/thread.h"

// Step 1: ADM. Step 2: renderers. Later: sources, device state.
class MediaPipeline {
 public:
  MediaPipeline(const webrtc::Environment& env,
                webrtc::Thread* worker_thread);
  ~MediaPipeline();

  // ---- ADM ----
  bool CreateAudioDeviceModule();
  webrtc::AudioDeviceModule* adm() const { return adm_.get(); }

  // ---- Renderers ----
  void StartLocalRenderer(webrtc::VideoTrackInterface* track);
  void StopLocalRenderer();
  void StartRemoteRenderer(webrtc::VideoTrackInterface* track);
  void StopRemoteRenderer();
  void StartRemoteAudioRenderer(webrtc::AudioTrackInterface* track);
  void StopRemoteAudioRenderer();

  // ---- Lifecycle ----
  void Shutdown();

 private:
  const webrtc::Environment& env_;
  webrtc::Thread* const worker_thread_;
  webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm_;

  std::unique_ptr<ShmVideoRenderer> local_renderer_;
  std::unique_ptr<ShmVideoRenderer> remote_renderer_;
  std::unique_ptr<ShmAudioRenderer> remote_audio_renderer_;
};

#endif  // APPS_PEERCONNECTION_CLIENT_MEDIA_PIPELINE_H_
