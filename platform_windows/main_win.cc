// platform_windows/main_win.cc — Windows platform entry point
//
// Differences from Linux main.cc:
//   - NamedPipeTransport instead of UnixSocketTransport
//   - Windows file logging paths
//   - No SSL init needed (WebRTC handles internally on Windows)
//
// Build: gn gen out/win --args='target_cpu="x64" target_os="win"'
//        ninja -C out/win platform_windows:client_win
#include <iostream>
#include <memory>
#include <string>

#include "api/environment/environment.h"
#include "api/environment/environment_factory.h"
#include "apps/client_x64_linux/media_pipeline.h"        // shared media pipeline (采集是跨平台的)
#include "apps/client_x64_linux/pc_factory_x64.h"        // x64 SW codecs (same on Windows)
#include "apps/platform_windows/pipe_transport_win.h"
#include "apps/webrtc_engine/control_protocol.h"
#include "apps/webrtc_engine/defaults.h"
#include "apps/webrtc_engine/flag_defs.h"
#include "apps/webrtc_engine/peer_connection_client.h"
#include "apps/webrtc_engine/pipe_transport_interface.h"
#include "apps/webrtc_engine/webrtc_engine.h"
#include "rtc_base/logging.h"
#include "rtc_base/physical_socket_server.h"
#include "rtc_base/thread.h"

int main(int argc, char* argv[]) {
  webrtc::PhysicalSocketServer pss;
  auto main_thread = std::make_unique<webrtc::Thread>(&pss);
  webrtc::ThreadManager::Instance()->SetCurrentThread(main_thread.get());

  webrtc::Environment env = webrtc::CreateEnvironment();

  // 1. Engine + platform dependencies
  auto engine = std::make_unique<WebRTCEngine>(env);
  if (!engine->Init()) {
    std::cerr << "Failed to initialize WebRTC engine" << std::endl;
    return 1;
  }
  engine->SetMediaPipeline(
      std::make_unique<MediaPipeline>(env, engine->worker_thread()));
  engine->SetPcFactory(std::make_unique<PcFactoryX64>());
  engine->SetSignaling(std::make_unique<PeerConnectionClient>());

  // 2. Named pipe transport (Windows-specific)
  std::string pipe_path = "/webrtc_ctrl";   // → \\.\pipe\webrtc_ctrl
  auto transport = std::make_unique<NamedPipeTransport>(pipe_path);

  // 3. Control protocol (platform-independent — same as Linux)
  auto protocol = std::make_unique<ControlProtocol>(transport.get(), engine.get());
  auto* proto_ptr = protocol.get();
  transport->on_message = [proto_ptr](const std::string& line) {
    proto_ptr->OnLineReceived(line);
  };
  protocol->on_shutdown = [&transport] { transport->Stop(); };
  engine->RegisterObserver(protocol.get());

  // 4. Start listening
  transport->Start(pipe_path);
  std::cout << "WebRTC Windows daemon started." << std::endl;
  transport->Run();

  engine->Shutdown();
  webrtc::ThreadManager::Instance()->SetCurrentThread(nullptr);
  return 0;
}
