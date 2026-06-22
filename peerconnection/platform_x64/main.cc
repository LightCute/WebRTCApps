// platform_x64/main.cc — x64 Linux platform entry point.
// Demonstrates dependency injection: engine ← platform implementations.
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "api/environment/environment.h"
#include "api/environment/environment_factory.h"
#include "apps/peerconnection/client/flag_defs.h"
#include "apps/peerconnection/client/media_pipeline.h"
#include "apps/peerconnection/client/webrtc_engine.h"
#include "apps/peerconnection/engine/peer_connection_client.h"
#include "apps/peerconnection/engine/signaling_interface.h"
#include "apps/peerconnection/platform_x64/pc_factory_x64.h"
#include "apps/peerconnection/platform_x64/shm_ipc_server.h"
#include "rtc_base/log_sinks.h"
#include "rtc_base/logging.h"
#include "rtc_base/physical_socket_server.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"

static std::atomic<bool> g_running{true};
static void sigint_handler(int) { g_running = false; }

int main(int argc, char* argv[]) {
  absl::ParseCommandLine(argc, argv);

  // ---- 1. WebRTC runtime ----
  auto pss = std::make_unique<webrtc::PhysicalSocketServer>();
  auto main_thread = std::make_unique<webrtc::Thread>(pss.get());
  webrtc::ThreadManager::Instance()->SetCurrentThread(main_thread.get());
  webrtc::InitializeSSL();

  webrtc::Environment env = webrtc::CreateEnvironment();

  // File logging
  std::string runtime_dir =
      absl::GetFlag(FLAGS_webtrc_runtime_dir);
  mkdir(runtime_dir.c_str(), 0755);
  webrtc::FileRotatingLogSink log_sink(runtime_dir, "webrtc_engine_x64",
                                       webrtc::FileRotatingLogSink::kMaxLogSize,
                                       2);
  log_sink.Enable();

  // ---- 2. Create engine ----
  auto engine = std::make_unique<WebRTCEngine>(env);
  engine->Init();  // creates signaling/worker/network threads

  // ---- 3. Create platform implementations ----
  auto pipeline = std::make_unique<MediaPipeline>(env, engine->worker_thread());
  auto pc_factory = std::make_unique<PcFactoryX64>();
  auto signaling = std::make_unique<PeerConnectionClient>();
  auto ipc = std::make_unique<ShmIpcServer>(runtime_dir);

  // ---- 4. Inject dependencies into engine ----
  engine->SetMediaPipeline(std::move(pipeline));
  engine->SetPcFactory(std::move(pc_factory));
  engine->SetSignaling(std::move(signaling));
  engine->SetIpcServer(std::move(ipc));

  // ---- 5. Connect to signaling server ----
  std::string server = absl::GetFlag(FLAGS_server);
  int port = absl::GetFlag(FLAGS_port);
  engine->ConnectToServer(server, port);

  std::cout << "WebRTC x64 Engine started." << std::endl;

  // ---- 6. Wait for shutdown ----
  std::signal(SIGINT, sigint_handler);
  std::signal(SIGTERM, sigint_handler);
  while (g_running) {
    sleep(1);
  }

  // ---- 7. Cleanup ----
  std::cout << "Shutting down..." << std::endl;
  engine->Shutdown();

  webrtc::CleanupSSL();
  webrtc::ThreadManager::Instance()->SetCurrentThread(nullptr);
  return 0;
}
