#ifndef APPS_WEBRTC_CLIENT_MEDIA_PULSE_QUERY_H_
#define APPS_WEBRTC_CLIENT_MEDIA_PULSE_QUERY_H_

#include <string>
#include <vector>

namespace webrtc_media {

// 一个 PulseAudio 录音源（已过滤 monitor 伪源）的硬件属性。
struct PulseSourceInfo {
  std::string name;         // Pulse source name（稳定，如 alsa_input.usb-...）
  std::string description;  // 描述（与 ADM 的 RecordingDeviceName 一致）
  std::string available;    // 端口可用性：yes / no / unknown
  int sample_rate = 0;      // 硬件采样率
  int channels = 0;         // 声道数
};

// 短生命周期连接查询 Pulse 源列表；服务不可用/超时返回空列表（不阻塞超过 2s）。
std::vector<PulseSourceInfo> QueryPulseSources();

}  // namespace webrtc_media

#endif  // APPS_WEBRTC_CLIENT_MEDIA_PULSE_QUERY_H_
