#include "apps/peerconnection/client_arm64/shm_audio_renderer.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunsafe-buffer-usage"
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"

#include <cstddef>
#include <utility>

#include "rtc_base/logging.h"

ShmAudioRenderer::ShmAudioRenderer(const std::string& key_path,
                                     int proj_id)
    : writer_(std::make_unique<ShmAudioWriter>()),
      io_thread_(&ShmAudioRenderer::IoLoop, this) {
  if (!writer_->Init(key_path, proj_id)) {
    RTC_LOG(LS_ERROR) << "ShmAudioRenderer: ShmAudioWriter::Init failed for "
                      << key_path;
  }
}

ShmAudioRenderer::~ShmAudioRenderer() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
  }
  cv_.notify_all();
  if (io_thread_.joinable()) {
    io_thread_.join();
  }
}

void ShmAudioRenderer::OnData(
    const void* audio_data, int bits_per_sample, int sample_rate,
    size_t number_of_channels, size_t number_of_frames,
    std::optional<int64_t> absolute_capture_timestamp_ms) {
  size_t byte_count =
      number_of_frames * number_of_channels * (bits_per_sample / 8);
  if (byte_count > AUDIO_FRAME_MAX_SIZE) {
    RTC_LOG(LS_WARNING) << "ShmAudioRenderer: frame too large: " << byte_count;
    return;
  }

  PendingAudio pending;
  pending.head.ntp_time_ms = absolute_capture_timestamp_ms.value_or(0);
  pending.head.frame_len = static_cast<uint32_t>(byte_count);
  pending.head.sample_rate = static_cast<uint32_t>(sample_rate);
  pending.head.channels = static_cast<uint16_t>(number_of_channels);
  pending.head.bits_per_sample = static_cast<uint16_t>(bits_per_sample);
  pending.data.assign(static_cast<const uint8_t*>(audio_data),
                      static_cast<const uint8_t*>(audio_data) + byte_count);

  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_ = std::move(pending);
  }
  cv_.notify_one();
}

void ShmAudioRenderer::IoLoop() {
  while (true) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return pending_.has_value() || stopped_; });
    if (stopped_) {
      return;
    }
    if (pending_.has_value()) {
      auto pending = std::move(*pending_);
      pending_.reset();
      lock.unlock();
      writer_->WriteFrame(pending.head, pending.data.data());
    }
  }
}

#pragma GCC diagnostic pop
