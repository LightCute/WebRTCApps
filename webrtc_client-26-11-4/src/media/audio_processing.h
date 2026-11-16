#ifndef APPS_WEBRTC_CLIENT_MEDIA_AUDIO_PROCESSING_H_
#define APPS_WEBRTC_CLIENT_MEDIA_AUDIO_PROCESSING_H_

#include <memory>
#include <string>

#include "api/audio/audio_processing.h"
#include "api/peer_connection_interface.h"
#include "api/scoped_refptr.h"

namespace webrtc_media {

// 音频处理开关的简化视图（旧接口兼容 / 状态回显）。
struct AudioProcessingState {
  bool aec = false;  // 只采集阶段本地没有播放参考信号，默认关
  bool ns = true;
  bool agc = true;
};

// 音频源参数（AudioOptions）：改动后存 pending，下次建音轨（audio_on / 下次
// call）时生效，见实现说明。
struct AudioSourceOptions {
  bool stereo_swapping = false;
  int jitter_max_packets = 200;
  bool jitter_fast_accelerate = false;
  int jitter_min_delay_ms = 0;
  bool init_recording_on_send = true;
};

// 把「能捕获 APM 实例」的 builder 装进 factory deps；
// 构建出的 AudioProcessing 会写到 out_apm，供运行时 ApplyConfig 使用。
void ConfigureAudioProcessing(
    webrtc::PeerConnectionFactoryDependencies& deps,
    webrtc::scoped_refptr<webrtc::AudioProcessing>* out_apm);

// 引擎默认音频处理配置：
//   aec=off, ns=on(kHigh), agc=on(kAdaptiveAnalog), hpf=on, transient=off,
//   agc2=off, pre_amp/capture_level_adjustment=off。
webrtc::AudioProcessing::Config DefaultAudioProcessingConfig();

// 按 dotted key 修改配置（例如 "noise_suppression.level"）。
// 支持别名 aec/ns/ns_level/agc/agc_mode/hpf/transient/agc2。
// 成功返回 true；未知 key、非法值或越界返回 false 并写 error。
// 冲突自动裁决：AGC2 优先于 AGC1，capture_level_adjustment 优先于
// pre_amplifier；AEC 强制 HPF 时 hpf=true。
bool SetAudioProcessingKey(webrtc::AudioProcessing::Config* config,
                           const std::string& key, const std::string& value,
                           std::string* error);

// 按 dotted key 修改音频源参数（source.*），下次建音轨生效。
bool SetAudioSourceOption(AudioSourceOptions* options, const std::string& key,
                          const std::string& value, std::string* error);

// 运行时应用整份配置（worker 线程调用；无需重新协商）。
bool ApplyAudioProcessingConfig(webrtc::AudioProcessing* apm,
                                const webrtc::AudioProcessing::Config& config);

// 从完整配置推导旧的三开关视图。
AudioProcessingState StateFromConfig(
    const webrtc::AudioProcessing::Config& config);

// 旧接口：只改 AEC/NS/AGC 三个开关。
bool ApplyAudioProcessing(webrtc::AudioProcessing* apm,
                          const AudioProcessingState& state);

}  // namespace webrtc_media

#endif  // APPS_WEBRTC_CLIENT_MEDIA_AUDIO_PROCESSING_H_
