// Media pipeline abstraction — platform-specific capture/encode/render.
#ifndef APPS_PEERCONNECTION_ENGINE_MEDIA_PIPELINE_INTERFACE_H_
#define APPS_PEERCONNECTION_ENGINE_MEDIA_PIPELINE_INTERFACE_H_

#include <functional>
#include <memory>
#include <string>

#include "api/audio/audio_device.h"
#include "api/media_stream_interface.h"
#include "api/peer_connection_interface.h"
#include "api/scoped_refptr.h"
#include "api/video/video_source_interface.h"

class IMediaPipeline {
 public:
  using EventCallback = std::function<void(const std::string& json)>;

  virtual ~IMediaPipeline() = default;

  // ---- Lifecycle ----
  virtual void Shutdown() = 0;

  // ---- Audio ----
  virtual bool CreateAudioDeviceModule() = 0;
  virtual webrtc::AudioDeviceModule* adm() const = 0;
  virtual webrtc::AudioSourceInterface* CreateAudioSource(
      webrtc::PeerConnectionFactoryInterface* factory) = 0;

  // ---- Video ----
  virtual webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface>
  CreateVideoSource() = 0;

  // ---- Renderers ----
  virtual void StartLocalRenderer(webrtc::VideoTrackInterface* track) = 0;
  virtual void StopLocalRenderer() = 0;
  virtual void StartRemoteRenderer(webrtc::VideoTrackInterface* track) = 0;
  virtual void StopRemoteRenderer() = 0;

  // ---- Events ----
  virtual void SetEventCallback(EventCallback cb) = 0;

  // ---- Device management ----
  virtual void set_video_device_idx(int idx) = 0;
};

#endif  // APPS_PEERCONNECTION_ENGINE_MEDIA_PIPELINE_INTERFACE_H_
