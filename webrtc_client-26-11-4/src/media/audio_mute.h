#ifndef APPS_WEBRTC_CLIENT_MEDIA_AUDIO_MUTE_H_
#define APPS_WEBRTC_CLIENT_MEDIA_AUDIO_MUTE_H_

#include <atomic>
#include <memory>
#include <mutex>

#include "api/audio/audio_frame_processor.h"

namespace webrtc_media {

// 软件麦克风静音：在采集帧进入编码前把采样清零（RTP 继续发静音包），
// 只影响本进程，不像 ADM 的 SetMicrophoneMute 那样把整个 Pulse source 静音。
class MuteFrameProcessor : public webrtc::AudioFrameProcessor {
 public:
  explicit MuteFrameProcessor(std::shared_ptr<std::atomic<bool>> muted);
  ~MuteFrameProcessor() override = default;

  void Process(std::unique_ptr<webrtc::AudioFrame> frame) override;
  void SetSink(OnAudioFrameCallback sink_callback) override;

 private:
  const std::shared_ptr<std::atomic<bool>> muted_;
  std::mutex sink_mutex_;
  OnAudioFrameCallback sink_;
};

}  // namespace webrtc_media

#endif  // APPS_WEBRTC_CLIENT_MEDIA_AUDIO_MUTE_H_
