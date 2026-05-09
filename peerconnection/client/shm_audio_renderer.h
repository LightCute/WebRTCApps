#ifndef APPS_PEERCONNECTION_CLIENT_SHM_AUDIO_RENDERER_H_
#define APPS_PEERCONNECTION_CLIENT_SHM_AUDIO_RENDERER_H_

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "api/media_stream_interface.h"
#include "apps/peerconnection/client/shm_audio_writer.h"
#include "apps/peerconnection/client/shm_common.h"

// Renders remote audio to shared memory for an external Qt process.
// Runs a dedicated IO thread to avoid blocking the WebRTC signaling thread.
class ShmAudioRenderer : public webrtc::AudioTrackSinkInterface {
 public:
  ShmAudioRenderer(const std::string& key_path, int proj_id);
  ~ShmAudioRenderer() override;
  void OnData(const void* audio_data, int bits_per_sample,
              int sample_rate, size_t number_of_channels,
              size_t number_of_frames,
              std::optional<int64_t> absolute_capture_timestamp_ms) override;

 private:
  struct PendingAudio {
    AudioFrameHead head;
    std::vector<uint8_t> data;
  };

  void IoLoop();

  std::unique_ptr<ShmAudioWriter> writer_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::optional<PendingAudio> pending_;
  bool stopped_ = false;
  std::thread io_thread_;
};

#endif  // APPS_PEERCONNECTION_CLIENT_SHM_AUDIO_RENDERER_H_
