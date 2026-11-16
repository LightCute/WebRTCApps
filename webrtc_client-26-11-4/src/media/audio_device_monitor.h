#ifndef APPS_WEBRTC_CLIENT_MEDIA_AUDIO_DEVICE_MONITOR_H_
#define APPS_WEBRTC_CLIENT_MEDIA_AUDIO_DEVICE_MONITOR_H_

#include <atomic>
#include <functional>
#include <thread>

namespace webrtc_media {

// 音频设备热插拔监听：直接读内核 uevent（netlink，等价于 libudev monitor），
// 过滤 sound / usb 子系统，防抖后回调（引擎再重新枚举 Pulse 设备）。
class AudioDeviceMonitor {
 public:
  using Callback = std::function<void()>;

  AudioDeviceMonitor() = default;
  ~AudioDeviceMonitor();

  AudioDeviceMonitor(const AudioDeviceMonitor&) = delete;
  AudioDeviceMonitor& operator=(const AudioDeviceMonitor&) = delete;

  bool Start(Callback callback);
  void Stop();

 private:
  void Run();

  int fd_ = -1;
  std::atomic<bool> running_{false};
  std::thread thread_;
  Callback callback_;
};

}  // namespace webrtc_media

#endif  // APPS_WEBRTC_CLIENT_MEDIA_AUDIO_DEVICE_MONITOR_H_
