#include "logger.h"

#include <chrono>
#include <ctime>
#include <iostream>

namespace {

const char* LevelName(LogLevel level) {
  switch (level) {
    case LogLevel::kDebug:
      return "debug";
    case LogLevel::kInfo:
      return "info";
    case LogLevel::kWarn:
      return "warn";
    case LogLevel::kError:
      return "error";
  }
  return "?";
}

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

std::string FormatLine(LogLevel level, const std::string& msg) {
  return "[" + CurrentTimestamp() + "] [" + LevelName(level) + "] " + msg +
         "\n";
}

}  // namespace

void StdoutLogger::Log(LogLevel level, const std::string& msg) {
  std::cout << FormatLine(level, msg);
  std::cout.flush();
}

FileLogger::FileLogger(const std::string& path, size_t max_size, int max_files)
    : path_(path), max_size_(max_size), max_files_(max_files) {
  if (!path_.empty()) {
    file_.open(path_, std::ios::app);
  }
}

void FileLogger::Rotate() {
  file_.close();
  std::remove((path_ + "." + std::to_string(max_files_)).c_str());
  for (int i = max_files_ - 1; i >= 1; i--) {
    std::string src = path_ + "." + std::to_string(i);
    std::string dst = path_ + "." + std::to_string(i + 1);
    std::rename(src.c_str(), dst.c_str());
  }
  std::rename(path_.c_str(), (path_ + ".1").c_str());
  file_.open(path_, std::ios::trunc);
  bytes_written_ = 0;
}

void FileLogger::Log(LogLevel level, const std::string& msg) {
  if (path_.empty()) {
    return;
  }
  std::string line = FormatLine(level, msg);
  if (bytes_written_ > 0 && bytes_written_ + line.size() > max_size_) {
    Rotate();
  }
  if (file_.is_open()) {
    file_ << line;
    file_.flush();
    bytes_written_ += line.size();
  }
}

void MultiLogger::Log(LogLevel level, const std::string& msg) {
  for (Logger* s : sinks_) {
    s->Log(level, msg);
  }
}
