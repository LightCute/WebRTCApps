#ifndef APPS_WEBRTC_CLIENT_MEDIA_AUDIO_DEVICE_H_
#define APPS_WEBRTC_CLIENT_MEDIA_AUDIO_DEVICE_H_

#include <string>
#include <vector>

#include "api/audio/audio_device.h"
#include "api/environment/environment.h"
#include "api/scoped_refptr.h"

namespace webrtc_media {

// 一个录音设备（PulseAudio source）。
struct AudioDeviceInfo {
  int index = 0;      // ADM 内部索引（热插拔后可能变化）
  std::string id;     // 稳定 id：Pulse source name（如 alsa_input.usb-...）；默认别名="default"
  std::string name;   // 设备描述（ADM RecordingDeviceName，可能重名）
  std::string available = "unknown";  // 端口可用性：yes / no / unknown
  int hardware_sample_rate = 0;       // 硬件采样率（0 = 未知）
  int hardware_channels = 0;          // 硬件声道数（0 = 未知）
};

// 创建平台 ADM（PulseAudio 后端）并 Init；必须在 worker 线程调用。
// 失败返回 nullptr。
webrtc::scoped_refptr<webrtc::AudioDeviceModule> CreatePlatformAudioDeviceModule(
    const webrtc::Environment& env);

// 枚举录音设备（worker 线程）。WebRTC 的 Pulse 实现已过滤 monitor 伪设备。
std::vector<AudioDeviceInfo> EnumerateRecordingDevices(
    webrtc::AudioDeviceModule* adm);

// 按设备名设置录音设备（worker 线程；录音未初始化时才允许）。
bool SetRecordingDeviceByName(webrtc::AudioDeviceModule* adm,
                              const std::string& name);

}  // namespace webrtc_media

#endif  // APPS_WEBRTC_CLIENT_MEDIA_AUDIO_DEVICE_H_
