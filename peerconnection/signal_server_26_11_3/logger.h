#ifndef SIGNAL_SERVER_LOGGER_H_
#define SIGNAL_SERVER_LOGGER_H_

#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

enum class LogLevel { kDebug, kInfo, kWarn, kError };

// Abstract log sink.
class Logger {
 public:
  virtual ~Logger() = default;
  virtual void Log(LogLevel level, const std::string& msg) = 0;

  void Debug(const std::string& m) { Log(LogLevel::kDebug, m); }
  void Info(const std::string& m) { Log(LogLevel::kInfo, m); }
  void Warn(const std::string& m) { Log(LogLevel::kWarn, m); }
  void Error(const std::string& m) { Log(LogLevel::kError, m); }
};

// Writes "[<ts>] [<level>] msg" to stdout.
class StdoutLogger : public Logger {
 public:
  void Log(LogLevel level, const std::string& msg) override;
};

// Appends "[<ts>] [<level>] msg" to a file, rotating by size.
// If path is empty, this logger is a no-op.
class FileLogger : public Logger {
 public:
  FileLogger(const std::string& path, size_t max_size, int max_files);
  void Log(LogLevel level, const std::string& msg) override;

 private:
  void Rotate();

  std::string path_;
  size_t max_size_;
  int max_files_;
  std::ofstream file_;
  size_t bytes_written_ = 0;
};

// Forwards every log line to all wrapped sinks.
class MultiLogger : public Logger {
 public:
  explicit MultiLogger(std::initializer_list<Logger*> sinks) : sinks_(sinks) {}
  void Log(LogLevel level, const std::string& msg) override;

 private:
  std::vector<Logger*> sinks_;
};

#endif  // SIGNAL_SERVER_LOGGER_H_
