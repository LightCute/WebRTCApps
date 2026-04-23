/*
 *  Copyright 2012 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include <cstdio>
#include <memory>
#include <string>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "api/environment/environment.h"
#include "api/environment/environment_factory.h"
#include "api/field_trials.h"
#include "api/make_ref_counted.h"
#include "api/scoped_refptr.h"
#include "api/units/time_delta.h"
#include "apps/peerconnection/client/conductor.h"
#include "apps/peerconnection/client/flag_defs.h"
#include "apps/peerconnection/client/linux_cli/main_cli.h" // 🔥 包含 CLI 头文件
#include "apps/peerconnection/client/peer_connection_client.h"
#include "rtc_base/physical_socket_server.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"

// 🔥 自定义 SocketServer（适配 CLI）
class CliSocketServer : public webrtc::PhysicalSocketServer {
 public:
  explicit CliSocketServer(CliMainWnd* wnd)
      : wnd_(wnd), conductor_(nullptr), client_(nullptr) {}
  ~CliSocketServer() override {}

  void SetMessageQueue(webrtc::Thread* queue) override {
    message_queue_ = queue;
  }

  void set_client(PeerConnectionClient* client) { client_ = client; }
  void set_conductor(Conductor* conductor) { conductor_ = conductor; }

  // bool Wait(webrtc::TimeDelta max_wait_duration, bool process_io) override {
  //   if (!wnd_->IsWindow() && !conductor_->connection_active() &&
  //       client_ != nullptr && !client_->is_connected()) {
  //     message_queue_->Quit();
  //   }
  //   return webrtc::PhysicalSocketServer::Wait(webrtc::TimeDelta::Zero(), process_io);
  // }

 protected:
  webrtc::Thread* message_queue_;
  CliMainWnd* wnd_;
  Conductor* conductor_;
  PeerConnectionClient* client_;
};

int main(int argc, char* argv[]) {
  // 🔥 移除 gtk_init
  std::cout << "Starting PeerConnectionClient CLI..." << std::endl;
  absl::ParseCommandLine(argc, argv);

  webrtc::Environment env =
      webrtc::CreateEnvironment(std::make_unique<webrtc::FieldTrials>(
          absl::GetFlag(FLAGS_force_fieldtrials)));

  if ((absl::GetFlag(FLAGS_port) < 1) || (absl::GetFlag(FLAGS_port) > 65535)) {
    printf("Error: %i is not a valid port.\n", absl::GetFlag(FLAGS_port));
    return -1;
  }

  const std::string server = absl::GetFlag(FLAGS_server);
  
  // 🔥 替换 GtkMainWnd 为 CliMainWnd
  CliMainWnd wnd(server.c_str(), absl::GetFlag(FLAGS_port),
                 absl::GetFlag(FLAGS_autoconnect),
                 absl::GetFlag(FLAGS_autocall));

  // 🔥 替换 CustomSocketServer 为 CliSocketServer
  CliSocketServer socket_server(&wnd);
  std::unique_ptr<webrtc::Thread> thread =
      std::make_unique<webrtc::Thread>(&socket_server);
  webrtc::ThreadManager::Instance()->SetCurrentThread(thread.get());
  wnd.set_signaling_thread(thread.get());
  webrtc::InitializeSSL();
  
  PeerConnectionClient client;
  auto conductor = webrtc::make_ref_counted<Conductor>(env, &client, &wnd);
  socket_server.set_client(&client);
  socket_server.set_conductor(conductor.get());

  thread->PostTask([&wnd]() {
    wnd.Run();
  });

  // 🔥 运行 WebRTC 线程（这会阻塞）
  thread->Run();
  

  webrtc::ThreadManager::Instance()->SetCurrentThread(nullptr);

  webrtc::CleanupSSL();
  return 0;
}