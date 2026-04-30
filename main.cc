// main.cc
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "api/environment/environment.h"
#include "api/environment/environment_factory.h"
#include "api/field_trials.h"
#include "apps/peerconnection/client/cli_runner.h"
#include "apps/peerconnection/client/flag_defs.h"
#include "apps/peerconnection/client/unix_socket_server.h"
#include "apps/peerconnection/client/webrtc_engine.h"
#include "rtc_base/physical_socket_server.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"

ABSL_FLAG(bool, standalone, false, "Run in standalone CLI mode (no Unix socket)");

int main(int argc, char* argv[]) {
  absl::ParseCommandLine(argc, argv);

  webrtc::Environment env = webrtc::CreateEnvironment(
      std::make_unique<webrtc::FieldTrials>(
          absl::GetFlag(FLAGS_force_fieldtrials)));

  bool standalone = absl::GetFlag(FLAGS_standalone);

  // Set up a WebRTC main thread (PeerConnectionClient needs CurrentThread())
  webrtc::PhysicalSocketServer pss;
  auto main_thread = std::make_unique<webrtc::Thread>(&pss);
  webrtc::ThreadManager::Instance()->SetCurrentThread(main_thread.get());
  webrtc::InitializeSSL();

  // Create engine with no-op callback (set later)
  WebRTCEngine engine(env, nullptr);

  if (!engine.Init()) {
    std::cerr << "Failed to initialize WebRTC engine" << std::endl;
    webrtc::CleanupSSL();
    return 1;
  }

  if (standalone) {
    CliRunner cli(&engine,
                  absl::GetFlag(FLAGS_server),
                  absl::GetFlag(FLAGS_port),
                  absl::GetFlag(FLAGS_autoconnect),
                  absl::GetFlag(FLAGS_autocall));
    // Run CLI input loop on the main thread, event loop in a task
    main_thread->PostTask([&] { cli.Run(); });
    main_thread->Run();
  } else {
    UnixSocketServer unix_server("/tmp/webrtc_ctrl.sock", &engine);
    engine.SetEventCallback([&](const std::string& json) {
      unix_server.SendToClient(json);
    });
    unix_server.Start();
    std::cout << "WebRTC daemon started. Listening on /tmp/webrtc_ctrl.sock" << std::endl;
    unix_server.Wait();
  }

  engine.Shutdown();
  webrtc::ThreadManager::Instance()->SetCurrentThread(nullptr);
  webrtc::CleanupSSL();
  return 0;
}
