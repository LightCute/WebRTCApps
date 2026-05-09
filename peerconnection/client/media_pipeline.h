#ifndef APPS_PEERCONNECTION_CLIENT_MEDIA_PIPELINE_H_
#define APPS_PEERCONNECTION_CLIENT_MEDIA_PIPELINE_H_

#include <functional>
#include <memory>
#include <string>

#include "api/audio/audio_device.h"
#include "api/environment/environment.h"
#include "api/media_stream_interface.h"
#include "api/peer_connection_interface.h"
#include "api/scoped_refptr.h"
#include "apps/peerconnection/client/shm_audio_renderer.h"
#include "apps/peerconnection/client/shm_video_renderer.h"
#include "rtc_base/thread.h"

// Owns all media-related objects: ADM, sources, SHM renderers, device state.
// Engine calls pipeline methods; pipeline manages the lifecycle internally.
class MediaPipeline {
 public:
  using EventCallback = std::function<void(const std::string& json)>;

  MediaPipeline(const webrtc::Environment& env,
                webrtc::Thread* worker_thread);
  ~MediaPipeline();

  void SetEventCallback(EventCallback cb);

  // ---- ADM ----
  bool CreateAudioDeviceModule();
  webrtc::AudioDeviceModule* adm() const { return adm_.get(); }

  // ---- Sources (called during AddTracks) ----
  webrtc::AudioSourceInterface* CreateAudioSource(
      webrtc::PeerConnectionFactoryInterface* factory,
      webrtc::AudioSourceInterface* external_source = nullptr);
  webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> CreateVideoSource();

  // ---- Renderers ----
  void StartLocalRenderer(webrtc::VideoTrackInterface* track);
  void StopLocalRenderer();
  void StartRemoteRenderer(webrtc::VideoTrackInterface* track);
  void StopRemoteRenderer();
  void StartRemoteAudioRenderer(webrtc::AudioTrackInterface* track);
  void StopRemoteAudioRenderer();

  // ---- Device management ----
  void QueryDevices();
  void SetVideoDevice(int device_idx);
  void SetAudioInputDevice(int device_idx);

  // ---- Media control ----
  void SetAudioMuted(bool muted);
  void SetVideoPaused(bool paused, webrtc::PeerConnectionInterface* pc);

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
  EventCallback event_cb_;
};

#endif  // APPS_PEERCONNECTION_CLIENT_MEDIA_PIPELINE_H_
