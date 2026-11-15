#ifndef APPS_WEBRTC_CLIENT_MEDIA_AUDIO_PROCESSING_H_
#define APPS_WEBRTC_CLIENT_MEDIA_AUDIO_PROCESSING_H_

#include <memory>

#include "api/audio/audio_processing.h"
#include "api/peer_connection_interface.h"
#include "api/scoped_refptr.h"

namespace webrtc_media {

// 音频处理开关：AEC 回声消除 / NS 降噪 / AGC 自动增益。
struct AudioProcessingState {
  bool aec = false;  // 只采集阶段本地没有播放参考信号，默认关
  bool ns = true;
  bool agc = true;
};

// 把「能捕获 APM 实例」的 builder 装进 factory deps；
// 构建出的 AudioProcessing 会写到 out_apm，供运行时 ApplyConfig 使用。
void ConfigureAudioProcessing(
    webrtc::PeerConnectionFactoryDependencies& deps,
    webrtc::scoped_refptr<webrtc::AudioProcessing>* out_apm);

// 运行时应用开关（worker 线程调用；无需重新协商）。
bool ApplyAudioProcessing(webrtc::AudioProcessing* apm,
                          const AudioProcessingState& state);

}  // namespace webrtc_media

#endif  // APPS_WEBRTC_CLIENT_MEDIA_AUDIO_PROCESSING_H_
