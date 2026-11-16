#include "media/audio_device_monitor.h"

#include <linux/netlink.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <string_view>

#include "rtc_base/logging.h"

namespace webrtc_media {

namespace {
// USB 声卡插拔会连续产生 sound/usb 多个事件，合并成一次重新枚举。
constexpr auto kDebounce = std::chrono::milliseconds(500);
}  // namespace

AudioDeviceMonitor::~AudioDeviceMonitor() { Stop(); }

bool AudioDeviceMonitor::Start(Callback callback) {
  if (running_) {
    return false;
  }
  fd_ = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
  if (fd_ < 0) {
    RTC_LOG(LS_WARNING) << "AudioDeviceMonitor: netlink socket failed";
    return false;
  }
  sockaddr_nl addr;
  memset(&addr, 0, sizeof(addr));
  addr.nl_family = AF_NETLINK;
  addr.nl_groups = 1;  // 内核 uevent 组
  if (bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    RTC_LOG(LS_WARNING) << "AudioDeviceMonitor: netlink bind failed";
    ::close(fd_);
    fd_ = -1;
    return false;
  }
  callback_ = std::move(callback);
  running_ = true;
  thread_ = std::thread(&AudioDeviceMonitor::Run, this);
  RTC_LOG(LS_INFO) << "AudioDeviceMonitor started (netlink uevent)";
  return true;
}

void AudioDeviceMonitor::Stop() {
  if (!running_.exchange(false)) {
    return;
  }
  if (fd_ >= 0) {
    // 唤醒阻塞在 poll 上的线程。
    shutdown(fd_, SHUT_RDWR);
  }
  if (thread_.joinable()) {
    thread_.join();
  }
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  RTC_LOG(LS_INFO) << "AudioDeviceMonitor stopped";
}

void AudioDeviceMonitor::Run() {
  char buffer[4096];
  bool pending = false;
  auto last_event = std::chrono::steady_clock::now();

  while (running_) {
    pollfd pfd;
    pfd.fd = fd_;
    pfd.events = POLLIN;
    pfd.revents = 0;
    const int rc = poll(&pfd, 1, 200);

    if (rc > 0 && (pfd.revents & POLLIN)) {
      const ssize_t n = recv(fd_, buffer, sizeof(buffer), 0);
      if (n > 0) {
        // uevent 是 NUL 分隔的 "KEY=VALUE" 序列。
        const std::string_view payload(buffer, static_cast<size_t>(n));
        bool relevant = false;
        size_t pos = 0;
        while (pos < payload.size()) {
          size_t end = payload.find('\0', pos);
          if (end == std::string_view::npos) {
            end = payload.size();
          }
          const std::string_view line = payload.substr(pos, end - pos);
          // USB 音频拔插有时只发 usb 事件、不发 sound 事件，两者都接受；
          // 引擎侧会对比设备集合，只有真正变化才上报。
          if (line == "SUBSYSTEM=sound" || line == "SUBSYSTEM=usb") {
            relevant = true;
          }
          pos = end + 1;
        }
        if (relevant) {
          pending = true;
          last_event = std::chrono::steady_clock::now();
        }
      }
    }

    if (pending &&
        std::chrono::steady_clock::now() - last_event >= kDebounce) {
      pending = false;
      if (callback_ && running_) {
        callback_();
      }
    }
  }
}

}  // namespace webrtc_media
