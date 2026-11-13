#ifndef APPS_WEBRTC_CLIENT_MEDIA_VIDEO_DEVICE_H_
#define APPS_WEBRTC_CLIENT_MEDIA_VIDEO_DEVICE_H_

#include <string>
#include <vector>

// 视频采集设备枚举（V4L2 真实设备 + 内置合成设备）。
namespace webrtc_media {

struct VideoDeviceInfo {
  std::string id;            // "v4l2:<bus_info>" | "synthetic" | "synthetic_hd"
  std::string name;          // 人类可读名
  bool is_synthetic = false;  // true = 合成测试图（无需硬件）
  int width = 640;
  int height = 480;
  int fps = 30;
  int v4l2_index = -1;       // 真实设备：枚举时的 V4L2 序号（-1=合成设备）
};

// 一种像素格式支持的尺寸/帧率（能力查询结果）。
struct VideoSizeCapability {
  int width = 0;
  int height = 0;
  std::vector<int> fps;   // 支持的帧率（升序）
};

struct VideoFormatCapability {
  std::string pixel_format;   // "MJPG" / "YUYV" / "I420" ...
  std::vector<VideoSizeCapability> sizes;
};

// 枚举全部可用设备：真实设备（V4L2 只统计可采集节点）+ 内置合成设备。
// 合成设备永远存在（headless 机器也能调试）。
std::vector<VideoDeviceInfo> EnumerateVideoDevices();

// 按 id 查找设备（找不到返回 false）。
bool FindVideoDevice(const std::string& id, VideoDeviceInfo* out);

// 查询设备的采集能力（格式 → 尺寸 → 帧率）。真实设备走 V4L2 ioctl 枚举；
// 合成设备返回其默认尺寸。失败返回空列表。
std::vector<VideoFormatCapability> GetVideoDeviceCapabilities(
    const VideoDeviceInfo& dev);

// 按 WebRTC V4L2 capturer 的偏好规则（>640x480 优先 MJPG，否则 YUYV 等），
// 返回请求该尺寸时**实际会使用**的像素格式；空 = 设备没有可用格式。
std::string EffectivePixelFormat(const std::vector<VideoFormatCapability>& caps,
                                 int width, int height);

// 该能力表是否支持指定的 尺寸+帧率（按**实际生效格式**判断，与采集行为一致）。
bool IsProfileSupported(const std::vector<VideoFormatCapability>& caps,
                        int width, int height, int fps);

// 用 unique id 重新定位当前 V4L2 序号（拔插后序号会漂移）；找不到返回 -1。
int ResolveV4l2Index(const VideoDeviceInfo& dev);

}  // namespace webrtc_media

#endif  // APPS_WEBRTC_CLIENT_MEDIA_VIDEO_DEVICE_H_
