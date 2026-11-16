#include "media/audio_mute.h"

#include "api/audio/audio_frame.h"
#include "rtc_base/logging.h"

namespace webrtc_media {

MuteFrameProcessor::MuteFrameProcessor(
    std::shared_ptr<std::atomic<bool>> muted)
    : muted_(std::move(muted)) {}

void MuteFrameProcessor::Process(std::unique_ptr<webrtc::AudioFrame> frame) {
  if (!frame) {
    return;
  }
  if (muted_ && muted_->load(std::memory_order_relaxed)) {
    frame->Mute();
  }
  OnAudioFrameCallback sink;
  {
    std::lock_guard<std::mutex> lock(sink_mutex_);
    sink = sink_;
  }
  if (sink) {
    sink(std::move(frame));
  }
}

void MuteFrameProcessor::SetSink(OnAudioFrameCallback sink_callback) {
  std::lock_guard<std::mutex> lock(sink_mutex_);
  sink_ = std::move(sink_callback);
}

}  // namespace webrtc_media
