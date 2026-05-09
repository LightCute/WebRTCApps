#ifndef APPS_PEERCONNECTION_CLIENT_MEDIA_MANAGER_H_
#define APPS_PEERCONNECTION_CLIENT_MEDIA_MANAGER_H_

#include <functional>
#include <memory>
#include <string>

#include "api/audio/audio_device.h"
#include "api/environment/environment.h"
#include "api/media_stream_interface.h"
#include "api/peer_connection_interface.h"
#include "api/scoped_refptr.h"
#include "rtc_base/thread.h"

class MediaManager {
 public:
  using EventCallback = std::function<void(const std::string& json)>;

  MediaManager(const webrtc::Environment& env,
               webrtc::Thread* worker_thread);
  ~MediaManager();

  // ---- ADM lifecycle ----
  bool CreateAudioDeviceModule();
  webrtc::AudioDeviceModule* GetADM() const;

  // ---- Track creation ----
  // external_audio_source: if non-null, used instead of creating ADM source
  bool AddTracks(webrtc::PeerConnectionFactoryInterface* factory,
                 webrtc::PeerConnectionInterface* pc,
                 webrtc::AudioSourceInterface* external_audio_source = nullptr);

  // ---- Device enumeration ----
  void QueryDevices();

  // ---- Device selection ----
  void SetVideoDevice(int device_idx);
  void SetAudioInputDevice(int device_idx);

  // ---- Media control ----
  void SetAudioMuted(bool muted);
  void SetVideoPaused(bool paused, webrtc::PeerConnectionInterface* pc);

  // ---- Events ----
  void SetEventCallback(EventCallback cb);

  // ---- Lifecycle ----
  void Shutdown();

 private:
  const webrtc::Environment& env_;
  webrtc::Thread* const worker_thread_;

  webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm_;
  webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> video_source_;
  webrtc::scoped_refptr<webrtc::AudioSourceInterface> audio_source_;

  int video_device_idx_ = -1;
  int audio_input_device_idx_ = -1;
  EventCallback event_cb_;
};

#endif  // APPS_PEERCONNECTION_CLIENT_MEDIA_MANAGER_H_
