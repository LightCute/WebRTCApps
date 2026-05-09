#ifndef APPS_PEERCONNECTION_CLIENT_MEDIA_PIPELINE_H_
#define APPS_PEERCONNECTION_CLIENT_MEDIA_PIPELINE_H_

#include <memory>

#include "api/audio/audio_device.h"
#include "api/environment/environment.h"
#include "api/media_stream_interface.h"
#include "api/peer_connection_interface.h"
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

  // ---- Sources ----
  webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> CreateVideoSource();
  webrtc::AudioSourceInterface* CreateAudioSource(
      webrtc::PeerConnectionFactoryInterface* factory,
      webrtc::AudioSourceInterface* external_source = nullptr);

  // ---- Device management ----
  void SetVideoDevice(int device_idx);

  void set_video_device_idx(int idx) { video_device_idx_ = idx; }
  void set_audio_input_device_idx(int idx) { audio_input_device_idx_ = idx; }

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
  webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> video_source_;
  webrtc::scoped_refptr<webrtc::AudioSourceInterface> audio_source_;

  std::unique_ptr<ShmVideoRenderer> local_renderer_;
  std::unique_ptr<ShmVideoRenderer> remote_renderer_;
  std::unique_ptr<ShmAudioRenderer> remote_audio_renderer_;

  int video_device_idx_ = -1;
  int audio_input_device_idx_ = -1;
};

#endif  // APPS_PEERCONNECTION_CLIENT_MEDIA_PIPELINE_H_
