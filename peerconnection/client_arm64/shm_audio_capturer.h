#ifndef APPS_PEERCONNECTION_CLIENT_SHM_AUDIO_CAPTURER_H_
#define APPS_PEERCONNECTION_CLIENT_SHM_AUDIO_CAPTURER_H_

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "api/media_stream_interface.h"
#include "api/scoped_refptr.h"
#include "apps/peerconnection/client_arm64/shm_audio_reader.h"

// Reads captured audio from shared memory (produced by an external Qt process).
// Implements AudioSourceInterface so it can be used as a drop-in audio source
// for PeerConnectionFactory::CreateAudioTrack().
class ShmAudioCapturer : public webrtc::AudioSourceInterface {
 public:
  static webrtc::scoped_refptr<ShmAudioCapturer> Create(
      const std::string& key_path, int proj_id);
  ~ShmAudioCapturer() override;

  void AddSink(webrtc::AudioTrackSinkInterface* sink) override;
  void RemoveSink(webrtc::AudioTrackSinkInterface* sink) override;
  void RegisterObserver(webrtc::ObserverInterface*) override {}
  void UnregisterObserver(webrtc::ObserverInterface*) override {}
  webrtc::MediaSourceInterface::SourceState state() const override {
    return webrtc::MediaSourceInterface::kLive;
  }
  bool remote() const override { return false; }

 protected:
  ShmAudioCapturer() = default;
  void CaptureLoop();

  ShmAudioReader reader_;
  std::mutex sinks_mutex_;
  std::vector<webrtc::AudioTrackSinkInterface*> sinks_;
  std::thread capture_thread_;
  std::atomic<bool> running_{false};
};

#endif  // APPS_PEERCONNECTION_CLIENT_SHM_AUDIO_CAPTURER_H_
