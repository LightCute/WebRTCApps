// main.cc
#include <cstdio>
#include <dirent.h>
#include <iostream>
#include <memory>
#include <string>
#include <sys/stat.h>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "api/environment/environment.h"
#include "api/environment/environment_factory.h"
#include "api/field_trials.h"
#include "apps/client_arm64_linux/flag_defs.h"
#include "apps/client_arm64_linux/unix_socket_server.h"
#include "apps/client_arm64_linux/webrtc_engine.h"
#include "rtc_base/log_sinks.h"
#include "rtc_base/physical_socket_server.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"


int main(int argc, char* argv[]) {
  absl::ParseCommandLine(argc, argv);

  webrtc::Environment env = webrtc::CreateEnvironment(
      std::make_unique<webrtc::FieldTrials>(
          absl::GetFlag(FLAGS_force_fieldtrials)));


  // Set up a WebRTC main thread (PeerConnectionClient needs CurrentThread())
  webrtc::PhysicalSocketServer pss;
  auto main_thread = std::make_unique<webrtc::Thread>(&pss);
  webrtc::ThreadManager::Instance()->SetCurrentThread(main_thread.get());
  webrtc::InitializeSSL();

  // All runtime files (socket, SHM, logs) live under WEBRTC_RUNTIME_DIR.
  const char* rt_dir_env = getenv("WEBRTC_RUNTIME_DIR");
  std::string runtime_dir = rt_dir_env ? rt_dir_env : "/tmp/webrtc_runtime";
  mkdir(runtime_dir.c_str(), 0755);

  // Find next available log index so restarts don't overwrite crash logs
  int log_index = 0;
  {
    DIR* dir = opendir(runtime_dir.c_str());
    if (dir) {
      struct dirent* ent;
      while ((ent = readdir(dir))) {
        int n = 0;
        if (sscanf(ent->d_name, "daemon_%d.", &n) == 1 && n >= log_index)
          log_index = n + 1;
      }
      closedir(dir);
    }
  }
  std::string log_prefix = "daemon_" + std::to_string(log_index);
  webrtc::FileRotatingLogSink log_sink(runtime_dir, log_prefix,
                                        10 * 1024 * 1024, 5);
  log_sink.Init();
  log_sink.DisableBuffering();  // flush every line for crash triage
  webrtc::LogMessage::AddLogToStream(&log_sink, webrtc::LS_INFO);
  RTC_LOG(LS_INFO) << "Logging to: " << runtime_dir << "/"
                   << log_prefix << ".0.log";

  WebRTCEngine engine(env);

  if (!engine.Init()) {
    std::cerr << "Failed to initialize WebRTC engine" << std::endl;
    webrtc::CleanupSSL();
    return 1;
  }

  std::string sock_path = runtime_dir + "/webrtc_ctrl.sock";
  UnixSocketServer unix_server(sock_path, &engine);
  engine.RegisterObserver(&unix_server);
  unix_server.Start();
  std::cout << "WebRTC daemon started. Listening on " << sock_path << std::endl;
  unix_server.Wait();

  engine.Shutdown();
  webrtc::ThreadManager::Instance()->SetCurrentThread(nullptr);
  webrtc::CleanupSSL();

  webrtc::LogMessage::RemoveLogToStream(&log_sink);
  return 0;
}
