// media_pipeline test — validates video + audio capture → SHM output pipeline
#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <span>
#include <string>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "api/audio/audio_device_defines.h"
#include "api/audio/audio_device.h"
#include "api/environment/environment.h"
#include "api/environment/environment_factory.h"
#include "api/video/video_sink_interface.h"
#include "api/video/video_source_interface.h"
#include "apps/peerconnection/media_pipeline/media_pipeline.h"
#include "apps/peerconnection/media_pipeline/shm_video_renderer.h"
#include "rtc_base/logging.h"
#include "rtc_base/physical_socket_server.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"

ABSL_FLAG(int, device_idx, -1, "V4L2 device index (-1 = auto-select first)");
ABSL_FLAG(std::string, shm_key, "/tmp/webrtc_runtime/shm_video_buf",
          "Path used for ftok() key derivation");
ABSL_FLAG(int, shm_proj_id, 0x88, "Project ID for ftok()");
ABSL_FLAG(bool, no_audio, false, "Skip AudioDeviceModule creation");
ABSL_FLAG(int, audio_layer, 0,
          "AudioDeviceModule layer: 0=Default, 3=ALSA, 4=PulseAudio");

static std::atomic<bool> g_running{true};
static void sigint_handler(int) { g_running = false; }

// ===== Audio loopback transport =====
// Bridges recorded audio (mic) → playout (speaker) for live monitoring.
// Uses a ring buffer to decouple the record and playout callbacks (both
// are called from the ADM's internal audio thread at 10ms intervals).
class AudioLoopbackTransport : public webrtc::AudioTransport {
 public:
  static constexpr size_t kSampleRate = 48000;
  static constexpr size_t kChannels = 1;          // mono
  static constexpr size_t kBytesPerSample = 2;    // int16_t
  static constexpr size_t kFrameMs = 10;
  static constexpr size_t kSamplesPerFrame =
      kSampleRate * kFrameMs / 1000;              // 480 samples
  static constexpr size_t kRingBufFrames = 10;    // 100ms total buffer

  int32_t RecordedDataIsAvailable(const void* audioSamples,
                                  size_t nSamples,
                                  size_t nBytesPerSample,
                                  size_t nChannels,
                                  uint32_t samplesPerSec,
                                  uint32_t /* totalDelayMS */,
                                  int32_t /* clockDrift */,
                                  uint32_t /* currentMicLevel */,
                                  bool /* keyPressed */,
                                  uint32_t& newMicLevel) override {
    newMicLevel = 0;
    if (nSamples == 0) return 0;

    std::lock_guard<std::mutex> lock(mutex_);
    // Drop oldest frame if ring buffer is full (back-pressure)
    if (ring_buf_.size() >= kRingBufFrames) {
      ring_buf_.pop_front();
    }
    size_t bytes = nSamples * nBytesPerSample * nChannels;
    std::vector<uint8_t> frame(bytes);
    std::memcpy(frame.data(), audioSamples, bytes);
    ring_buf_.push_back(std::move(frame));

    if (first_record_) {
      first_record_ = false;
      RTC_LOG(LS_INFO) << "Audio record started: " << nSamples << " samples, "
                       << nChannels << " ch, " << samplesPerSec << " Hz";
    }
    return 0;
  }

  int32_t NeedMorePlayData(size_t nSamples,
                           size_t nBytesPerSample,
                           size_t nChannels,
                           uint32_t samplesPerSec,
                           void* audioSamples,
                           size_t& nSamplesOut,
                           int64_t* elapsed_time_ms,
                           int64_t* ntp_time_ms) override {
    *elapsed_time_ms = 0;
    *ntp_time_ms = 0;

    std::lock_guard<std::mutex> lock(mutex_);
    if (ring_buf_.empty()) {
      // No recorded data yet → output silence
      nSamplesOut = 0;
      return 0;
    }

    // Pop oldest frame and copy to playout buffer
    const auto& frame = ring_buf_.front();
    size_t expected = nSamples * nBytesPerSample * nChannels;
    size_t copy_bytes = std::min(expected, frame.size());
    std::memcpy(audioSamples, frame.data(), copy_bytes);
    if (copy_bytes < expected) {
      auto remainder = std::span(
          static_cast<uint8_t*>(audioSamples), expected).subspan(copy_bytes);
      std::ranges::fill(remainder, 0);
    }
    nSamplesOut = nSamples;
    ring_buf_.pop_front();

    if (first_playout_) {
      first_playout_ = false;
      RTC_LOG(LS_INFO) << "Audio playout started: " << nSamples << " samples, "
                       << nChannels << " ch, " << samplesPerSec << " Hz";
    }
    return 0;
  }

  void PullRenderData(int /* bits_per_sample */,
                      int /* sample_rate */,
                      size_t /* number_of_channels */,
                      size_t /* number_of_frames */,
                      void* /* audio_data */,
                      int64_t* /* elapsed_time_ms */,
                      int64_t* /* ntp_time_ms */) override {
    // Not used for loopback — render data is served via NeedMorePlayData
  }

 private:
  std::mutex mutex_;
  std::deque<std::vector<uint8_t>> ring_buf_;
  bool first_record_ = true;
  bool first_playout_ = true;
};

int main(int argc, char* argv[]) {
  absl::ParseCommandLine(argc, argv);

  int device_idx = absl::GetFlag(FLAGS_device_idx);
  std::string shm_key = absl::GetFlag(FLAGS_shm_key);
  int shm_proj_id = absl::GetFlag(FLAGS_shm_proj_id);
  bool no_audio = absl::GetFlag(FLAGS_no_audio);
  int audio_layer = absl::GetFlag(FLAGS_audio_layer);

  std::cout << "media_pipeline_test starting" << std::endl;
  std::cout << "  device_idx = " << device_idx << std::endl;
  std::cout << "  shm_key    = " << shm_key << std::endl;
  std::cout << "  shm_proj_id = 0x" << std::hex << shm_proj_id
            << std::dec << std::endl;
  std::cout << "  no_audio   = " << (no_audio ? "true" : "false") << std::endl;

  // 1. WebRTC environment
  auto pss = std::make_unique<webrtc::PhysicalSocketServer>();
  auto thread = std::make_unique<webrtc::Thread>(pss.get());
  webrtc::ThreadManager::Instance()->SetCurrentThread(thread.get());
  webrtc::InitializeSSL();

  webrtc::Environment env = webrtc::CreateEnvironment();

  // 2. Create MediaPipeline
  MediaPipeline pipeline(env, thread.get());

  // 3. Audio: ADM + loopback transport (mic → speaker monitoring)
  AudioLoopbackTransport audio_transport;
  if (!no_audio && pipeline.CreateAudioDeviceModule(audio_layer)) {
    webrtc::AudioDeviceModule* adm = pipeline.adm();
    std::cout << "AudioDeviceModule created successfully" << std::endl;

    if (adm->Init() == 0) {
      // Must select devices before InitSpeaker/InitMicrophone,
      // otherwise _outputDeviceIsSpecified/_inputDeviceIsSpecified are false
      // and InitSpeaker/InitMicrophone return -1 immediately.
      adm->SetPlayoutDevice(0);    // 0 = default speaker
      adm->SetRecordingDevice(0);  // 0 = default microphone

      // Init microphone (recording)
      if (adm->InitMicrophone() == 0) {
        uint32_t vol = 0;
        adm->MicrophoneVolume(&vol);
        std::cout << "  Microphone initialized, volume=" << vol << std::endl;
      } else {
        std::cout << "  Microphone init failed (no mic?)" << std::endl;
      }

      // Init speaker (playout)
      if (adm->InitSpeaker() == 0) {
        uint32_t vol = 0;
        adm->SpeakerVolume(&vol);
        std::cout << "  Speaker initialized, volume=" << vol << std::endl;
      } else {
        std::cout << "  Speaker init failed (no speaker?)" << std::endl;
      }

      // Register the loopback callback
      adm->RegisterAudioCallback(&audio_transport);

      // Start recording + playout
      int rec_err = adm->StartRecording();
      int play_err = adm->StartPlayout();
      if (rec_err == 0 && play_err == 0) {
        std::cout << "  Audio loopback active (mic → speaker)" << std::endl;
      } else {
        std::cout << "  Audio loopback not started: rec=" << rec_err
                  << " play=" << play_err << " (no HW?)" << std::endl;
      }
    } else {
      std::cout << "ADM Init() failed" << std::endl;
    }
  } else if (!no_audio) {
    std::cout << "AudioDeviceModule creation failed (expected if no audio HW)"
              << std::endl;
  }

  // 4. Create video source
  webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> source =
      pipeline.CreateVideoSource();
  if (!source) {
    std::cerr << "Failed to create video source" << std::endl;
    webrtc::CleanupSSL();
    return 1;
  }
  std::cout << "Video source created" << std::endl;

  // 5. Create SHM renderer and attach directly to source
  ShmVideoRenderer sink(shm_key, shm_proj_id);
  webrtc::VideoSinkWants wants;
  source->AddOrUpdateSink(&sink, wants);
  std::cout << "SHM renderer attached to video source" << std::endl;
  std::cout << "Frames are being written to: " << shm_key << std::endl;
  std::cout << "Press Ctrl+C to stop." << std::endl;

  // 6. Wait for SIGINT
  std::signal(SIGINT, sigint_handler);
  std::signal(SIGTERM, sigint_handler);
  while (g_running) {
    sleep(1);
  }

  // 7. Graceful shutdown
  std::cout << "\nShutting down..." << std::endl;
  source->RemoveSink(&sink);
  pipeline.Shutdown();

  webrtc::CleanupSSL();
  webrtc::ThreadManager::Instance()->SetCurrentThread(nullptr);

  std::cout << "Done." << std::endl;
  return 0;
}
