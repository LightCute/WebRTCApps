#include "rtc_log_sink.h"

#include <chrono>
#include <cstdio>
#include <ctime>

namespace {

std::string CurrentTimestamp() {
  auto now = std::chrono::system_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch())
                .count();
  std::time_t sec = static_cast<std::time_t>(ms / 1000);
  int millis = static_cast<int>(ms % 1000);
  std::tm tm_now{};
  localtime_r(&sec, &tm_now);
  char date_part[32];
  std::strftime(date_part, sizeof(date_part), "%Y-%m-%d %H:%M:%S", &tm_now);
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%s.%03d", date_part, millis);
  return std::string(buf);
}

const char* SeverityName(webrtc::LoggingSeverity sev) {
  switch (sev) {
    case webrtc::LS_INFO:
      return "info";
    case webrtc::LS_WARNING:
      return "warn";
    case webrtc::LS_ERROR:
      return "error";
    default:
      return "?";
  }
}

}  // namespace

RtcLogSink::RtcLogSink(const std::string& log_file) {
  if (!log_file.empty()) {
    file_.open(log_file, std::ios::out | std::ios::app);
  }
}

RtcLogSink::~RtcLogSink() = default;

void RtcLogSink::OnLogMessage(const std::string& message) {
  // 旧 API（无 file:line）：只加时间戳。
  Write("[" + CurrentTimestamp() + "] " + message);
}

void RtcLogSink::OnLogMessage(const webrtc::LogLineRef& line) {
  std::string message(line.message());
  while (!message.empty() &&
         (message.back() == '\n' || message.back() == '\r')) {
    message.pop_back();  // message 自带换行，去掉避免 Write 再补一个空行
  }
  std::string out = "[" + CurrentTimestamp() + "] [" +
                    SeverityName(line.severity()) + "] (" +
                    std::string(line.filename()) + ":" +
                    std::to_string(line.line()) + ") " + message;
  Write(out);
}

void RtcLogSink::Write(const std::string& line) {
  std::lock_guard<std::mutex> lock(mu_);
  fprintf(stderr, "%s\n", line.c_str());
  if (file_.is_open()) {
    file_ << line << "\n";
    file_.flush();
  }
}
