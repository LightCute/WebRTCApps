#ifndef APPS_WEBRTC_CLIENT_MEDIA_VIDEO_DEVICE_MONITOR_H_
#define APPS_WEBRTC_CLIENT_MEDIA_VIDEO_DEVICE_MONITOR_H_

#include <atomic>
#include <functional>
#include <thread>
#include <vector>

#include "media/video_device.h"

// 视频设备热插拔监听：直接读内核 uevent（netlink，等价于 libudev monitor），
// 过滤 SUBSYSTEM=video4linux，防抖后重新枚举设备并回调。
namespace webrtc_media {

class VideoDeviceMonitor {
 public:
  using Callback = std::function<void(const std::vector<VideoDeviceInfo>&)>;

  VideoDeviceMonitor() = default;
  ~VideoDeviceMonitor();

  VideoDeviceMonitor(const VideoDeviceMonitor&) = delete;
  VideoDeviceMonitor& operator=(const VideoDeviceMonitor&) = delete;

  // 启动监听线程；失败（无法开 netlink socket）返回 false。
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

#endif  // APPS_WEBRTC_CLIENT_MEDIA_VIDEO_DEVICE_MONITOR_H_
