// SHM IPC server — wraps UnixSocketServer, implements IIpcServer.
#ifndef APPS_PEERCONNECTION_PLATFORM_X64_SHM_IPC_SERVER_H_
#define APPS_PEERCONNECTION_PLATFORM_X64_SHM_IPC_SERVER_H_

#include <memory>
#include <string>

#include "apps/client_x64_linux/unix_socket_server.h"
#include "apps/webrtc_engine/ipc_server_interface.h"

class ShmIpcServer : public IIpcServer {
 public:
  explicit ShmIpcServer(const std::string& runtime_dir);
  ~ShmIpcServer() override = default;

  bool Start(const std::string& path_or_address) override;
  void Run() override;
  void SendEvent(const std::string& json) override;
  void SetCommandHandler(CommandHandler handler) override;

 private:
  std::unique_ptr<UnixSocketServer> socket_;
};

#endif  // APPS_PEERCONNECTION_PLATFORM_X64_SHM_IPC_SERVER_H_
