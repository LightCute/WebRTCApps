#ifndef APPS_PEERCONNECTION_WEBRTC_CLIENT_RTC_LOG_SINK_H_
#define APPS_PEERCONNECTION_WEBRTC_CLIENT_RTC_LOG_SINK_H_

#include <fstream>
#include <mutex>
#include <string>

#include "rtc_base/logging.h"

// 统一日志落点：接收所有 RTC_LOG（含 RtcLogger 转来的），格式化为
// [年月日时分秒.毫秒] [level] (file:line) msg，同时写 stderr + 日志文件。
class RtcLogSink : public webrtc::LogSink {
 public:
  explicit RtcLogSink(const std::string& log_file);
  ~RtcLogSink() override;

  void OnLogMessage(const std::string& message) override;
  void OnLogMessage(const webrtc::LogLineRef& line) override;

 private:
  void Write(const std::string& line);

  std::ofstream file_;
  std::mutex mu_;
};

#endif  // APPS_PEERCONNECTION_WEBRTC_CLIENT_RTC_LOG_SINK_H_
