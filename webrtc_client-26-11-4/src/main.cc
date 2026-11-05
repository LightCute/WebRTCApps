#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <sys/stat.h>

#include "engine_controller.h"
#include "rtc_base/logging.h"
#include "rtc_log_sink.h"
#include "ipc/unix_socket_server.h"
#include "webrtc_engine.h"

using namespace webrtc;

namespace {

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

int ParseIntFlag(int argc, char* argv[], const char* name, int def) {
  return std::atoi(ParseStrFlag(argc, argv, name, std::to_string(def)).c_str());
}

std::string MakeLogFilePath(const std::string& log_dir) {
  mkdir(log_dir.c_str(), 0755);
  std::time_t now = std::time(nullptr);
  std::tm tm_now{};
  localtime_r(&now, &tm_now);
  char ts[32];
  std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm_now);
  return log_dir + "/webrtc_engine_" + ts + ".log";
}

}  // namespace

int main(int argc, char* argv[]) {
  std::string socket_path =
      ParseStrFlag(argc, argv, "socket_path", "/tmp/webrtc_engine.sock");
  std::string signal_server = ParseStrFlag(argc, argv, "server", "127.0.0.1");
  int signal_port = ParseIntFlag(argc, argv, "port", 8888);
  std::string log_dir =
      ParseStrFlag(argc, argv, "log_dir", "/tmp/webrtc_engine");

  // 日志：所有 RTC_LOG 统一走 RtcLogSink（时间戳 + file:line + stderr + 文件）。
  RtcLogSink log_sink(MakeLogFilePath(log_dir));
  LogMessage::SetLogToStderr(false);
  LogMessage::AddLogToStream(&log_sink, LS_INFO);

  webrtc_engine engine;
  if (!engine.Start()) {
    RTC_LOG(LS_ERROR) << "engine Start failed";
    return 1;
  }

  UnixSocketServer server(socket_path);
  EngineController controller(&engine, &server, signal_server, signal_port);
  engine.RegisterObserver(&controller);
  server.AddObserver(&controller);

  server.Start();
  RTC_LOG(LS_INFO) << "unix socket: " << socket_path
                   << ", signal server: " << signal_server << ":"
                   << signal_port;

  server.Wait();  // 阻塞直到 quit 命令

  server.Stop();
  engine.Stop();
  return 0;
}
