// audio_test — ADM mic-to-speaker loopback for NAU8822 ALSA validation
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <iostream>
#include <mutex>
#include <optional>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "api/audio/audio_device.h"
#include "api/audio/audio_device_defines.h"
#include "api/audio/create_audio_device_module.h"
#include "api/environment/environment.h"
#include "api/environment/environment_factory.h"
#include "api/field_trials.h"
#include "rtc_base/log_sinks.h"
#include "rtc_base/logging.h"
#include "rtc_base/thread.h"
#include "system_wrappers/include/clock.h"

ABSL_FLAG(int, duration, 30, "Loopback duration in seconds (0=infinite)");

namespace {

std::atomic<bool> g_running{true};

void SignalHandler(int) { g_running = false; }

// Lock-free single-producer single-consumer ring buffer for PCM frames.
// Write: RecordedDataIsAvailable callback. Read: NeedMorePlayData callback.
class PcmRingBuffer {
 public:
  static constexpr int kChannels = 2;
  static constexpr int kBytesPerSample = 2;  // S16_LE
  static constexpr int kFramesPerSlot = 480; // 10ms @ 48kHz
  static constexpr int kSlotBytes = kFramesPerSlot * kChannels * kBytesPerSample;
  static constexpr int kNumSlots = 8;

  PcmRingBuffer() : buf_(kNumSlots * kSlotBytes) {}

  // Returns bytes written (0 if full).
  size_t Write(const uint8_t* data, size_t bytes) {
    std::lock_guard<std::mutex> lock(mtx_);
    size_t avail = (write_idx_ >= read_idx_)
                       ? (kNumSlots - 1) - (write_idx_ - read_idx_)
                       : (read_idx_ - write_idx_ - 1);
    if (avail == 0) return 0;
    size_t n = std::min(bytes, (size_t)kSlotBytes);
    size_t offset = write_idx_ * kSlotBytes;
    memcpy(&buf_[offset], data, n);
    write_idx_ = (write_idx_ + 1) % kNumSlots;
    cv_.notify_one();
    return n;
  }

  // Returns bytes read.
  size_t Read(uint8_t* data, size_t max_bytes) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (write_idx_ == read_idx_) {
      memset(data, 0, max_bytes);
      return max_bytes;  // underrun: return silence
    }
    size_t n = std::min(max_bytes, (size_t)kSlotBytes);
    memcpy(data, &buf_[read_idx_ * kSlotBytes], n);
    read_idx_ = (read_idx_ + 1) % kNumSlots;
    cv_.notify_one();
    return n;
  }

  size_t available() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return (write_idx_ >= read_idx_) ? (write_idx_ - read_idx_)
                                     : (kNumSlots - read_idx_ + write_idx_);
  }

 private:
  mutable std::mutex mtx_;
  std::condition_variable cv_;
  std::vector<uint8_t> buf_;
  size_t write_idx_ = 0;
  size_t read_idx_ = 0;
};

class LoopbackTransport : public webrtc::AudioTransport {
 public:
  explicit LoopbackTransport(PcmRingBuffer& rb) : rb_(rb) {}

  int32_t RecordedDataIsAvailable(const void* audioSamples, size_t nSamples,
                                  size_t nBytesPerSample, size_t nChannels,
                                  uint32_t samplesPerSec,
                                  uint32_t totalDelayMS, int32_t clockDrift,
                                  uint32_t currentMicLevel, bool keyPressed,
                                  uint32_t& newMicLevel) override {
    size_t bytes = nSamples * nBytesPerSample * nChannels;
    rb_.Write(static_cast<const uint8_t*>(audioSamples), bytes);
    newMicLevel = currentMicLevel;
    return 0;
  }

  int32_t NeedMorePlayData(size_t nSamples, size_t nBytesPerSample,
                           size_t nChannels, uint32_t samplesPerSec,
                           void* audioSamples, size_t& nSamplesOut,
                           int64_t* elapsed_time_ms,
                           int64_t* ntp_time_ms) override {
    size_t bytes = nSamples * nBytesPerSample * nChannels;
    rb_.Read(static_cast<uint8_t*>(audioSamples), bytes);
    nSamplesOut = nSamples;
    *elapsed_time_ms = 0;
    *ntp_time_ms = 0;
    return 0;
  }

  void PullRenderData(int bits_per_sample, int sample_rate,
                      size_t number_of_channels, size_t number_of_frames,
                      void* audio_data, int64_t* elapsed_time_ms,
                      int64_t* ntp_time_ms) override {
    // Not used for loopback.
  }

 private:
  PcmRingBuffer& rb_;
};

// Select NAU8822 (card 1) for recording and playout.
// Must be called AFTER adm->Init() due to CHECKinitialized_() guards.
void SelectNau8822(webrtc::AudioDeviceModule* adm) {
  // Select recording device.
  int16_t rec_n = adm->RecordingDevices();
  RTC_LOG(LS_INFO) << "=== Device Enumeration ===";
  RTC_LOG(LS_INFO) << "RecordingDevices returned: " << rec_n;
  bool found = false;
  for (int16_t i = 0; i < rec_n; i++) {
    char name[webrtc::kAdmMaxDeviceNameSize];
    char guid[webrtc::kAdmMaxGuidSize];
    if (adm->RecordingDeviceName(i, name, guid) == 0) {
      RTC_LOG(LS_INFO) << "  RecDev[" << i << "]: name='" << name << "' guid='" << guid << "'";
      if (!found && (strstr(name, "1") || strstr(name, "nau8822") ||
                     strstr(name, "rockchipnau8822") ||
                     strstr(guid, "1") || strstr(guid, "nau8822"))) {
        int32_t ret = adm->SetRecordingDevice(i);
        RTC_LOG(LS_INFO) << "  -> SetRecordingDevice(" << i << ") returned " << ret;
        found = true;
      }
    }
  }
  if (!found) {
    RTC_LOG(LS_WARNING) << "NAU8822 not found in recording devices — check device names above";
  }

  // Select playout device.
  int16_t play_n = adm->PlayoutDevices();
  RTC_LOG(LS_INFO) << "PlayoutDevices returned: " << play_n;
  found = false;
  for (int16_t i = 0; i < play_n; i++) {
    char name[webrtc::kAdmMaxDeviceNameSize];
    char guid[webrtc::kAdmMaxGuidSize];
    if (adm->PlayoutDeviceName(i, name, guid) == 0) {
      RTC_LOG(LS_INFO) << "  PlayDev[" << i << "]: name='" << name << "' guid='" << guid << "'";
      if (!found && (strstr(name, "1") || strstr(name, "nau8822") ||
                     strstr(name, "rockchipnau8822") ||
                     strstr(guid, "1") || strstr(guid, "nau8822"))) {
        int32_t ret = adm->SetPlayoutDevice(i);
        RTC_LOG(LS_INFO) << "  -> SetPlayoutDevice(" << i << ") returned " << ret;
        found = true;
      }
    }
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  absl::ParseCommandLine(argc, argv);
  signal(SIGINT, SignalHandler);
  signal(SIGTERM, SignalHandler);

  // Log RTC_LOG to /tmp/audio_test.0.log so we can see ADM internals
  webrtc::FileRotatingLogSink log_sink("/tmp", "audio_test", 10*1024*1024, 3);
  log_sink.Init();
  log_sink.DisableBuffering();
  webrtc::LogMessage::AddLogToStream(&log_sink, webrtc::LS_INFO);
  RTC_LOG(LS_INFO) << "Logging to /tmp/audio_test.0.log";

  auto env = webrtc::CreateEnvironment(
      std::make_unique<webrtc::FieldTrials>(""));

  // --- Create ADM (ALSA direct if rtc_include_pulse_audio=false) ---
  auto adm = webrtc::CreateAudioDeviceModule(
      env, webrtc::AudioDeviceModule::kPlatformDefaultAudio);
  if (!adm) {
    std::cerr << "ERROR: Failed to create AudioDeviceModule" << std::endl;
    return 1;
  }

  // --- Phase 1: Init to satisfy CHECKinitialized_() ---
  {
    int32_t ret = adm->Init();
    RTC_LOG(LS_INFO) << "Phase1: adm->Init() returned " << ret;
    if (ret != 0) {
      std::cerr << "ERROR: ADM Init failed (ret=" << ret << ")" << std::endl;
      return 1;
    }
  }

  // --- Enumerate + select NAU8822 ---
  // Must happen AFTER Init() (CHECKinitialized_ guard) but BEFORE
  // InitMicrophone() (which uses _inputDeviceIsSpecified/_inputDeviceIndex).
  // Do NOT call Terminate() here — it resets _inputDeviceIsSpecified to false,
  // causing InitRecordingLocked() to return -1 immediately.
  SelectNau8822(adm.get());

  // --- Configure stereo ---
  // StereoRecordingIsAvailable() internally calls InitRecordingLocked() +
  // StopRecordingLocked() to probe — it does NOT leave recording initialized.
  // Same for StereoPlayoutIsAvailable. Use these only for detection;
  // explicitly call InitPlayout/InitRecording to persist the state.
  {
    bool stereo_avail = false;
    int32_t ret = adm->StereoRecordingIsAvailable(&stereo_avail);
    RTC_LOG(LS_INFO) << "StereoRecordingIsAvailable: ret=" << ret << " avail=" << stereo_avail;
    if (stereo_avail) {
      ret = adm->SetStereoRecording(true);
      RTC_LOG(LS_INFO) << "SetStereoRecording(true) returned " << ret;
    }
  }

  // --- Init playout and recording ---
  {
    int32_t ret = adm->InitPlayout();
    RTC_LOG(LS_INFO) << "InitPlayout returned " << ret;
    if (ret != 0) {
      std::cerr << "ERROR: InitPlayout failed (ret=" << ret << ")" << std::endl;
      return 1;
    }
  }
  {
    int32_t ret = adm->InitRecording();
    RTC_LOG(LS_INFO) << "InitRecording returned " << ret;
    if (ret != 0) {
      std::cerr << "ERROR: InitRecording failed (ret=" << ret << ")" << std::endl;
      return 1;
    }
  }

  // --- Register loopback transport ---
  PcmRingBuffer ring_buf;
  LoopbackTransport transport(ring_buf);
  adm->RegisterAudioCallback(&transport);

  // --- Start ---
  {
    int32_t ret = adm->StartRecording();
    RTC_LOG(LS_INFO) << "StartRecording returned " << ret;
    if (ret != 0) {
      std::cerr << "ERROR: StartRecording failed (ret=" << ret << ")" << std::endl;
      return 1;
    }
  }
  {
    int32_t ret = adm->StartPlayout();
    RTC_LOG(LS_INFO) << "StartPlayout returned " << ret;
    if (ret != 0) {
      std::cerr << "ERROR: StartPlayout failed (ret=" << ret << ")" << std::endl;
      return 1;
    }
  }

  int duration_s = absl::GetFlag(FLAGS_duration);
  auto start_time = webrtc::Clock::GetRealTimeClock()->CurrentTime();
  auto last_stats = start_time;

  std::cout << "Loopback running" << (duration_s ? " for " + std::to_string(duration_s) + "s" : "")
            << ". Press Ctrl+C to stop." << std::endl;

  while (g_running) {
    // Check g_running every 100ms instead of sleep(1) for responsive exit
    for (int i = 0; i < 10 && g_running; i++) {
      webrtc::Thread::SleepMs(100);
    }
    if (!g_running) break;

    auto now = webrtc::Clock::GetRealTimeClock()->CurrentTime();

    // Print ring buffer fill level every 10 seconds.
    if (now - last_stats >= webrtc::TimeDelta::Seconds(10)) {
      size_t slots = ring_buf.available();
      RTC_LOG(LS_INFO) << "Ring buffer: " << slots << "/8 slots filled";
      last_stats = now;
    }

    if (duration_s > 0 &&
        (now - start_time) >= webrtc::TimeDelta::Seconds(duration_s)) {
      break;
    }
  }

  std::cout << "Shutting down..." << std::endl;
  {
    int32_t ret = adm->StopRecording();
    RTC_LOG(LS_INFO) << "StopRecording returned " << ret;
  }
  {
    int32_t ret = adm->StopPlayout();
    RTC_LOG(LS_INFO) << "StopPlayout returned " << ret;
  }
  {
    int32_t ret = adm->Terminate();
    RTC_LOG(LS_INFO) << "Terminate returned " << ret;
  }
  std::cout << "Done." << std::endl;
  std::fflush(stdout);
  return 0;
}
