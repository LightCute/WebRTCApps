#include "apps/peerconnection/platform_x64/shm_ipc_server.h"

#include "rtc_base/logging.h"

ShmIpcServer::ShmIpcServer(const std::string& runtime_dir) {
  socket_ = std::make_unique<UnixSocketServer>(runtime_dir + "/dc_call.sock");
}

bool ShmIpcServer::Start(const std::string& /*path_or_address*/) {
  RTC_LOG(LS_INFO) << "ShmIpcServer: waiting for client...";
  return socket_->WaitForClient();
}

void ShmIpcServer::Run() {
  // UnixSocketServer-based IPC: client is already connected via WaitForClient.
  // Events are sent via SendEvent; the server itself doesn't need a run loop
  // in this design.
}

void ShmIpcServer::SendEvent(const std::string& json) {
  if (socket_ && socket_->IsConnected())
    socket_->WriteLine(json);
}

void ShmIpcServer::SetCommandHandler(CommandHandler handler) {
  // Stub — current design uses direct WebRTCEngine calls, not command handlers.
  (void)handler;
}
