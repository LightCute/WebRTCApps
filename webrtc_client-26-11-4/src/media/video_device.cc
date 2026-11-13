#include "media/video_device.h"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "modules/video_capture/video_capture.h"
#include "modules/video_capture/video_capture_factory.h"
#include "rtc_base/logging.h"

namespace webrtc_media {

namespace {

// fourcc → 可读格式名（"MJPG" / "YUYV" ...）。
std::string FourccToString(uint32_t fourcc) {
  std::string s(4, ' ');
  s[0] = static_cast<char>(fourcc & 0xff);
  s[1] = static_cast<char>((fourcc >> 8) & 0xff);
  s[2] = static_cast<char>((fourcc >> 16) & 0xff);
  s[3] = static_cast<char>((fourcc >> 24) & 0xff);
  return s;
}

// 枚举某个格式/尺寸下的帧率（离散值；区间型取端点）。
std::vector<int> EnumerateFps(int fd, uint32_t pixel_format, int width,
                              int height) {
  std::vector<int> fps;
  v4l2_frmivalenum fi;
  memset(&fi, 0, sizeof(fi));
  fi.pixel_format = pixel_format;
  fi.width = static_cast<uint32_t>(width);
  fi.height = static_cast<uint32_t>(height);
  for (fi.index = 0; ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &fi) == 0; ++fi.index) {
    if (fi.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
      if (fi.discrete.numerator > 0 && fi.discrete.denominator > 0) {
        fps.push_back(static_cast<int>(fi.discrete.denominator /
                                       fi.discrete.numerator));
      }
    } else if (fi.type == V4L2_FRMIVAL_TYPE_STEPWISE ||
               fi.type == V4L2_FRMIVAL_TYPE_CONTINUOUS) {
      // 区间型：取最小/最大帧率两个端点。
      if (fi.stepwise.min.numerator > 0 && fi.stepwise.min.denominator > 0) {
        fps.push_back(static_cast<int>(fi.stepwise.min.denominator /
                                       fi.stepwise.min.numerator));
      }
      if (fi.stepwise.max.numerator > 0 && fi.stepwise.max.denominator > 0) {
        fps.push_back(static_cast<int>(fi.stepwise.max.denominator /
                                       fi.stepwise.max.numerator));
      }
      break;
    }
  }
  std::sort(fps.begin(), fps.end());
  fps.erase(std::unique(fps.begin(), fps.end()), fps.end());
  return fps;
}

// bus_info（unique id）→ /dev/videoN 的节点号。
// 注意：WebRTC 的 DeviceInfo 序号是「过滤后」的序号（metadata 节点被跳过），
// 与节点号不一致（如 2 个摄像头时序号 0/1 对应 /dev/video0、/dev/video2），
// 所以能力查询必须按 bus_info 重新扫描节点，不能直接用序号。
int ResolveDeviceNode(const std::string& bus_info) {
  if (bus_info.empty()) {
    return -1;
  }
  for (int n = 0; n < 64; ++n) {
    char node[32];
    snprintf(node, sizeof(node), "/dev/video%d", n);
    const int fd = open(node, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
      continue;
    }
    v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    bool match = false;
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0 &&
        (cap.device_caps & V4L2_CAP_VIDEO_CAPTURE) &&
        bus_info == reinterpret_cast<const char*>(cap.bus_info)) {
      match = true;
    }
    close(fd);
    if (match) {
      return n;
    }
  }
  return -1;
}

// 内置合成设备（无摄像头时的调试源；synthetic_hd 用于验证切换生效）。
std::vector<VideoDeviceInfo> BuiltinSyntheticDevices() {
  VideoDeviceInfo sd;
  sd.id = "synthetic";
  sd.name = "Synthetic 640x480@30";
  sd.is_synthetic = true;
  sd.width = 640;
  sd.height = 480;
  sd.fps = 30;

  VideoDeviceInfo hd;
  hd.id = "synthetic_hd";
  hd.name = "Synthetic 1280x720@15";
  hd.is_synthetic = true;
  hd.width = 1280;
  hd.height = 720;
  hd.fps = 15;

  return {sd, hd};
}

}  // namespace

std::vector<VideoDeviceInfo> EnumerateVideoDevices() {
  std::vector<VideoDeviceInfo> devices;

  // DeviceInfoV4l2 内部按 V4L2_CAP_VIDEO_CAPTURE 过滤，metadata 节点（如
  // /dev/video1 的 META_CAPTURE）不会出现在列表里。
  std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> info(
      webrtc::VideoCaptureFactory::CreateDeviceInfo());
  if (info) {
    const int count = static_cast<int>(info->NumberOfDevices());
    RTC_LOG(LS_INFO) << "V4L2 capture device count: " << count;
    for (int i = 0; i < count; ++i) {
      char name[256] = {0};
      char unique_id[256] = {0};
      if (info->GetDeviceName(static_cast<uint32_t>(i), name, sizeof(name),
                              unique_id, sizeof(unique_id)) != 0) {
        RTC_LOG(LS_WARNING) << "GetDeviceName failed for index " << i;
        continue;
      }
      // bus_info 可能为空（个别驱动），退回序号作 id。
      const std::string uid = unique_id[0] != 0 ? unique_id : std::to_string(i);
      VideoDeviceInfo dev;
      dev.id = "v4l2:" + uid;
      dev.name = name[0] != 0 ? name : ("V4L2 camera " + std::to_string(i));
      dev.is_synthetic = false;
      dev.v4l2_index = i;
      RTC_LOG(LS_INFO) << "Video device[" << i << "]: id=" << dev.id
                       << " name=" << dev.name;
      devices.push_back(std::move(dev));
    }
  } else {
    RTC_LOG(LS_WARNING) << "VideoCaptureFactory::CreateDeviceInfo failed";
  }

  for (auto& s : BuiltinSyntheticDevices()) {
    devices.push_back(std::move(s));
  }
  return devices;
}

bool FindVideoDevice(const std::string& id, VideoDeviceInfo* out) {
  if (!out) {
    return false;
  }
  for (const auto& dev : EnumerateVideoDevices()) {
    if (dev.id == id) {
      *out = dev;
      return true;
    }
  }
  return false;
}

int ResolveV4l2Index(const VideoDeviceInfo& dev) {
  if (dev.is_synthetic) {
    return -1;
  }
  std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> info(
      webrtc::VideoCaptureFactory::CreateDeviceInfo());
  if (!info) {
    return -1;
  }
  const int count = static_cast<int>(info->NumberOfDevices());
  for (int i = 0; i < count; ++i) {
    char name[256] = {0};
    char unique_id[256] = {0};
    if (info->GetDeviceName(static_cast<uint32_t>(i), name, sizeof(name),
                            unique_id, sizeof(unique_id)) != 0) {
      continue;
    }
    const std::string uid = unique_id[0] != 0 ? unique_id : std::to_string(i);
    if ("v4l2:" + uid == dev.id) {
      return i;
    }
  }
  return -1;
}

std::vector<VideoFormatCapability> GetVideoDeviceCapabilities(
    const VideoDeviceInfo& dev) {
  std::vector<VideoFormatCapability> result;
  if (dev.is_synthetic) {
    VideoFormatCapability fmt;
    fmt.pixel_format = "I420";
    VideoSizeCapability size;
    size.width = dev.width;
    size.height = dev.height;
    size.fps = {dev.fps};
    fmt.sizes.push_back(std::move(size));
    result.push_back(std::move(fmt));
    return result;
  }

  // dev.id = "v4l2:<bus_info>"；按 bus_info 重新定位真实节点号。
  const std::string bus_info =
      dev.id.rfind("v4l2:", 0) == 0 ? dev.id.substr(5) : std::string();
  const int node_num = ResolveDeviceNode(bus_info);
  if (node_num < 0) {
    RTC_LOG(LS_WARNING) << "GetVideoDeviceCapabilities: device node not found: "
                        << dev.id;
    return result;
  }
  char node[32];
  snprintf(node, sizeof(node), "/dev/video%d", node_num);
  const int fd = open(node, O_RDWR | O_NONBLOCK);
  if (fd < 0) {
    RTC_LOG(LS_WARNING) << "GetVideoDeviceCapabilities: open failed: " << node;
    return result;
  }

  v4l2_fmtdesc fmtdesc;
  memset(&fmtdesc, 0, sizeof(fmtdesc));
  fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  for (fmtdesc.index = 0; ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) == 0;
       ++fmtdesc.index) {
    VideoFormatCapability fmt;
    fmt.pixel_format = FourccToString(fmtdesc.pixelformat);

    v4l2_frmsizeenum frmsize;
    memset(&frmsize, 0, sizeof(frmsize));
    frmsize.pixel_format = fmtdesc.pixelformat;
    for (frmsize.index = 0; ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) == 0;
         ++frmsize.index) {
      if (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
        VideoSizeCapability size;
        size.width = static_cast<int>(frmsize.discrete.width);
        size.height = static_cast<int>(frmsize.discrete.height);
        size.fps = EnumerateFps(fd, fmtdesc.pixelformat, size.width,
                                size.height);
        fmt.sizes.push_back(std::move(size));
      } else if (frmsize.type == V4L2_FRMSIZE_TYPE_STEPWISE ||
                 frmsize.type == V4L2_FRMSIZE_TYPE_CONTINUOUS) {
        // 区间型：取最大尺寸端点（常见的 4:3 / 16:9 由前端自行裁剪）。
        VideoSizeCapability size;
        size.width = static_cast<int>(frmsize.stepwise.max_width);
        size.height = static_cast<int>(frmsize.stepwise.max_height);
        size.fps = EnumerateFps(fd, fmtdesc.pixelformat, size.width,
                                size.height);
        fmt.sizes.push_back(std::move(size));
        break;
      }
    }
    result.push_back(std::move(fmt));
  }
  close(fd);
  return result;
}

std::string EffectivePixelFormat(const std::vector<VideoFormatCapability>& caps,
                                 int width, int height) {
  // 与 modules/video_capture/linux/video_capture_v4l2.cc:154-170 的偏好顺序一致。
  // 注意名字要用 V4L2 实际上报的 fourcc：YUV420/YVU420 分别是 "YU12"/"YV12"；
  // 末尾的 "I420" 是合成设备用的名字。
  static const std::vector<std::string> kHdPreference = {
      "MJPG",   "YU12", "YV12", "YUYV",  "UYVY",  "NV12",  "ABGR32",
      "ARGB32", "RGBA32", "BGR32",  "RGB32", "BGR24", "RGB24", "RGB565",
      "JPEG",   "I420"};
  static const std::vector<std::string> kSdPreference = {
      "YU12", "YV12", "YUYV",  "UYVY",  "NV12",  "ABGR32", "ARGB32",
      "RGBA32", "BGR32",  "RGB32", "BGR24", "RGB24", "RGB565", "MJPG",
      "JPEG", "I420"};
  const bool is_hd = (width > 640 || height > 480);
  const std::vector<std::string>& preference =
      is_hd ? kHdPreference : kSdPreference;
  for (const auto& name : preference) {
    for (const auto& fmt : caps) {
      if (fmt.pixel_format == name) {
        return name;
      }
    }
  }
  // 兜底：设备格式都不在偏好表里（如合成设备的 I420）时，取第一个可用格式。
  if (!caps.empty()) {
    return caps.front().pixel_format;
  }
  return "";
}

bool IsProfileSupported(const std::vector<VideoFormatCapability>& caps,
                        int width, int height, int fps) {
  const std::string effective = EffectivePixelFormat(caps, width, height);
  if (effective.empty()) {
    return false;
  }
  for (const auto& fmt : caps) {
    if (fmt.pixel_format != effective) {
      continue;
    }
    for (const auto& size : fmt.sizes) {
      if (size.width != width || size.height != height) {
        continue;
      }
      if (size.fps.empty()) {
        return true;  // 驱动没报帧率列表，不阻断
      }
      for (int f : size.fps) {
        if (f >= fps) {
          return true;  // 帧率不超过该格式的上限即可
        }
      }
      return false;  // 尺寸对，但该格式达不到请求的帧率
    }
    return false;  // 生效格式不支持这个尺寸
  }
  return false;
}

}  // namespace webrtc_media
