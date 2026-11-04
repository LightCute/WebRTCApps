#include <cstdlib>
#include <string>

#include "logger.h"
#include "signaling_server.h"

namespace {

int ParseIntFlag(int argc, char* argv[], const char* name, int def) {
  std::string prefix = std::string("--") + name + "=";
  std::string bare = std::string("--") + name;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a.rfind(prefix, 0) == 0) {
      return std::atoi(a.c_str() + prefix.size());
    }
    if (a == bare && i + 1 < argc) {
      return std::atoi(argv[i + 1]);
    }
  }
  return def;
}

std::string ParseStrFlag(int argc, char* argv[], const char* name,
                         const std::string& def) {
  std::string prefix = std::string("--") + name + "=";
  std::string bare = std::string("--") + name;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a.rfind(prefix, 0) == 0) {
      return a.substr(prefix.size());
    }
    if (a == bare && i + 1 < argc) {
      return argv[i + 1];
    }
  }
  return def;
}

}  // namespace

int main(int argc, char* argv[]) {
  int port = ParseIntFlag(argc, argv, "port", 8888);
  int ping_interval = ParseIntFlag(argc, argv, "ping_interval", 10);
  int ping_timeout = ParseIntFlag(argc, argv, "ping_timeout", 30);
  std::string log_file = ParseStrFlag(argc, argv, "log_file", "");
  int log_max_mb = ParseIntFlag(argc, argv, "log_max_size_mb", 10);
  int log_max_files = ParseIntFlag(argc, argv, "log_max_files", 3);

  StdoutLogger stdout_logger;
  FileLogger file_logger(log_file,
                         static_cast<size_t>(log_max_mb) * 1024 * 1024,
                         log_max_files);
  MultiLogger logger({&stdout_logger, &file_logger});

  SignalingServer server(port, ping_interval, ping_timeout, logger);
  server.Run();
  return 0;
}
