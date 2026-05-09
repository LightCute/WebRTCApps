#include "apps/peerconnection/client/shm_audio_capturer.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunsafe-buffer-usage"
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"

#include <cstddef>
#include <vector>

#include "api/make_ref_counted.h"
#include "rtc_base/logging.h"

webrtc::scoped_refptr<ShmAudioCapturer>
ShmAudioCapturer::Create(const std::string& key_path, int proj_id) {
  auto source = webrtc::make_ref_counted<ShmAudioCapturer>();
  if (!source->reader_.Init(key_path, proj_id)) {
    RTC_LOG(LS_ERROR) << "ShmAudioCapturer: ShmAudioReader::Init failed for "
                      << key_path;
    return nullptr;
  }
  source->running_ = true;
  source->capture_thread_ =
      std::thread(&ShmAudioCapturer::CaptureLoop, source.get());
  return source;
}

ShmAudioCapturer::~ShmAudioCapturer() {
  running_ = false;
  reader_.RequestStop();
  if (capture_thread_.joinable()) {
    capture_thread_.join();
  }
}

void ShmAudioCapturer::AddSink(webrtc::AudioTrackSinkInterface* sink) {
  std::lock_guard<std::mutex> lock(sinks_mutex_);
  sinks_.push_back(sink);
}

void ShmAudioCapturer::RemoveSink(webrtc::AudioTrackSinkInterface* sink) {
  std::lock_guard<std::mutex> lock(sinks_mutex_);
  sinks_.erase(std::remove(sinks_.begin(), sinks_.end(), sink), sinks_.end());
}

void ShmAudioCapturer::CaptureLoop() {
  std::vector<uint8_t> buf(AUDIO_FRAME_MAX_SIZE);
  while (running_) {
    AudioFrameHead head;
    if (!reader_.ReadFrame(head, buf.data(), AUDIO_FRAME_MAX_SIZE)) {
      break;
    }

    std::lock_guard<std::mutex> lock(sinks_mutex_);
    for (auto* sink : sinks_) {
      if (sink) {
        sink->OnData(buf.data(), head.bits_per_sample,
                     static_cast<int>(head.sample_rate), head.channels,
                     head.frame_len /
                         (head.channels * (head.bits_per_sample / 8)),
                     head.ntp_time_ms);
      }
    }
  }
}

#pragma GCC diagnostic pop
