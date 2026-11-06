#include "logger.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#define CHECK(cond)                                \
  do {                                             \
    if (!(cond)) {                                 \
      std::cerr << "FAIL: " #cond " @ line "       \
                << __LINE__ << "\n";               \
      return 1;                                    \
    }                                              \
  } while (0)

namespace {

bool file_exists(const std::string& path) {
  std::ifstream in(path);
  return in.good();
}

std::streampos file_size(const std::string& path) {
  std::ifstream in(path, std::ios::ate | std::ios::binary);
  return in.tellg();
}

std::string read_all(const std::string& path) {
  std::ifstream in(path);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

}  // namespace

class RecordingLogger : public Logger {
 public:
  void Log(LogLevel, const std::string& msg) override { calls_.push_back(msg); }
  std::vector<std::string> calls_;
};

int main() {
  // MultiLogger forwards to every sink
  RecordingLogger a, b;
  MultiLogger m({&a, &b});
  m.Info("hello");
  CHECK(a.calls_.size() == 1 && b.calls_.size() == 1);
  CHECK(a.calls_[0] == "hello");

  // FileLogger writes to file and rotates by size
  std::string path = "/tmp/signal_server_logger_test.log";
  std::remove(path.c_str());
  std::remove((path + ".1").c_str());
  std::remove((path + ".2").c_str());

  {
    FileLogger f(path, /*max_size=*/200, /*max_files=*/2);
    for (int i = 0; i < 40; i++) {
      f.Info("line number " + std::to_string(i));
    }
  }

  CHECK(file_exists(path));
  CHECK(file_exists(path + ".1"));   // 发生过滚动
  CHECK(file_size(path) <= 400);     // 当前文件有界，没把 40 行都堆进去
  std::string content = read_all(path);
  CHECK(content.find("line number 39") != std::string::npos);  // 最新一行在当前文件
  CHECK(content.find("line number 0") == std::string::npos);   // 最旧一行已被滚走

  std::cout << "logger_test: all pass\n";
  return 0;
}
