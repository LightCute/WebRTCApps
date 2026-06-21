// client_x64_linux/main.cc — x64 Linux platform entry point
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "api/environment/environment.h"
#include "api/environment/environment_factory.h"
#include "apps/client_x64_linux/media_pipeline.h"
#include "apps/client_x64_linux/pc_factory_x64.h"
#include "apps/client_x64_linux/unix_socket_server.h"
#include "apps/webrtc_engine/defaults.h"
#include "apps/webrtc_engine/flag_defs.h"
#include "apps/webrtc_engine/peer_connection_client.h"
#include "apps/webrtc_engine/webrtc_engine.h"
#include "rtc_base/log_sinks.h"
#include "rtc_base/logging.h"
#include "rtc_base/physical_socket_server.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"

static std::atomic<bool> g_running{true};
static void sigint_handler(int) { g_running = false; }

int main(int argc, char* argv[]) {
  absl::ParseCommandLine(argc, argv);

  // 1. WebRTC runtime
  auto pss = std::make_unique<webrtc::PhysicalSocketServer>();
  auto main_thread = std::make_unique<webrtc::Thread>(pss.get());
  webrtc::ThreadManager::Instance()->SetCurrentThread(main_thread.get());
  webrtc::InitializeSSL();

  webrtc::Environment env = webrtc::CreateEnvironment();

  std::string runtime_dir = "/tmp/webrtc_runtime";
  mkdir(runtime_dir.c_str(), 0755);
  webrtc::FileRotatingLogSink* log_sink = new webrtc::FileRotatingLogSink(
      runtime_dir, "webrtc_x64", 10 * 1024 * 1024, 2);
  webrtc::LogMessage::AddLogToStream(log_sink, webrtc::LS_INFO);

  // 2. Create engine + inject platform dependencies
  auto engine = std::make_unique<WebRTCEngine>(env);
  engine->Init();
  engine->SetMediaPipeline(
      std::make_unique<MediaPipeline>(env, engine->worker_thread()));
  engine->SetPcFactory(std::make_unique<PcFactoryX64>());
  engine->SetSignaling(std::make_unique<PeerConnectionClient>());

  // 3. Connect to signaling server
  engine->ConnectToServer(absl::GetFlag(FLAGS_server),
                          absl::GetFlag(FLAGS_port));

  // 4. Start IPC server for external client
  UnixSocketServer ipc(runtime_dir + "/dc_call.sock", engine.get());
  ipc.Start();

  std::cout << "WebRTC x64 Engine started. Ctrl+C to stop." << std::endl;

  // 5. Wait for shutdown
  std::signal(SIGINT, sigint_handler);
  std::signal(SIGTERM, sigint_handler);
  while (g_running) sleep(1);

  // 6. Cleanup
  std::cout << "Shutting down..." << std::endl;
  ipc.Stop();
  engine->Shutdown();
  webrtc::CleanupSSL();
  webrtc::ThreadManager::Instance()->SetCurrentThread(nullptr);
  return 0;
}
