#ifndef APPS_PEERCONNECTION_CLIENT_MEDIA_PIPELINE_H_
#define APPS_PEERCONNECTION_CLIENT_MEDIA_PIPELINE_H_

#include <memory>

#include "api/audio/audio_device.h"
#include "api/environment/environment.h"
#include "api/scoped_refptr.h"
#include "rtc_base/thread.h"

// Step 1: owns only AudioDeviceModule. Later steps will add more.
class MediaPipeline {
 public:
  MediaPipeline(const webrtc::Environment& env,
                webrtc::Thread* worker_thread);
  ~MediaPipeline();

  bool CreateAudioDeviceModule();
  webrtc::AudioDeviceModule* adm() const { return adm_.get(); }
  void Shutdown();

 private:
  const webrtc::Environment& env_;
  webrtc::Thread* const worker_thread_;
  webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm_;
};

#endif  // APPS_PEERCONNECTION_CLIENT_MEDIA_PIPELINE_H_
