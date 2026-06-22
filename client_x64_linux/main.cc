// client_x64_linux/main.cc — x64 Linux platform entry point (daemon mode)
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
#include "apps/client_x64_linux/media_pipeline.h"
#include "apps/client_x64_linux/pc_factory_x64.h"
#include "apps/client_x64_linux/pipe_transport_unix.h"
#include "apps/webrtc_engine/control_protocol.h"
#include "apps/webrtc_engine/defaults.h"
#include "apps/webrtc_engine/engine_controller.h"
#include "apps/webrtc_engine/flag_defs.h"
#include "apps/webrtc_engine/peer_connection_client.h"
#include "apps/webrtc_engine/pipe_transport_interface.h"
#include "apps/webrtc_engine/webrtc_engine.h"
#include "rtc_base/log_sinks.h"
#include "rtc_base/physical_socket_server.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"

int main(int argc, char* argv[]) {
  absl::ParseCommandLine(argc, argv);

  webrtc::Environment env = webrtc::CreateEnvironment(
      std::make_unique<webrtc::FieldTrials>(
          absl::GetFlag(FLAGS_force_fieldtrials)));

  webrtc::PhysicalSocketServer pss;
  auto main_thread = std::make_unique<webrtc::Thread>(&pss);
  webrtc::ThreadManager::Instance()->SetCurrentThread(main_thread.get());
  webrtc::InitializeSSL();

  const char* rt_dir_env = getenv("WEBRTC_RUNTIME_DIR");
  std::string runtime_dir = rt_dir_env ? rt_dir_env : "/tmp/webrtc_runtime";
  mkdir(runtime_dir.c_str(), 0755);

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
  log_sink.DisableBuffering();
  webrtc::LogMessage::AddLogToStream(&log_sink, webrtc::LS_INFO);
  RTC_LOG(LS_INFO) << "Logging to: " << runtime_dir << "/"
                   << log_prefix << ".0.log";

  // 1. Create engine + inject platform dependencies
  WebRTCEngine engine(env);
  if (!engine.Init()) {
    std::cerr << "Failed to initialize WebRTC engine" << std::endl;
    webrtc::CleanupSSL();
    return 1;
  }
  engine.SetMediaPipeline(
      std::make_unique<MediaPipeline>(env, engine.worker_thread()));
  engine.SetPcFactory(std::make_unique<PcFactoryX64>());
  engine.SetSignaling(std::make_unique<PeerConnectionClient>());

  // 2. Create pipe transport (platform-specific)
  std::string sock_path = runtime_dir + "/webrtc_ctrl.sock";
  auto transport = std::make_unique<UnixSocketTransport>(sock_path);

  // 3. Create control protocol (platform-independent JSON-RPC parser)
  ControlProtocol protocol(transport.get(), &engine);

  // 4. Wire transport → protocol → engine → transport
  transport->on_message = [&protocol](const std::string& line) {
    protocol.OnLineReceived(line);
  };
  transport->on_connected = [] {
    RTC_LOG(LS_INFO) << "Control client connected";
  };
  transport->on_disconnected = [] {
    RTC_LOG(LS_INFO) << "Control client disconnected";
  };
  protocol.on_shutdown = [&transport] { transport->Stop(); };

  // Engine observer — events go back to client via ControlProtocol::OnEngineEvent
  engine.RegisterObserver(&protocol);

  // 5. Start listening
  transport->Start(sock_path);

  std::cout << "WebRTC x64 daemon started. Listening on " << sock_path
            << std::endl;

  transport->Run();  // block until shutdown

  engine.Shutdown();
  webrtc::ThreadManager::Instance()->SetCurrentThread(nullptr);
  webrtc::CleanupSSL();
  webrtc::LogMessage::RemoveLogToStream(&log_sink);
  return 0;
}
