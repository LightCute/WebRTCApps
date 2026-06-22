// IPC server abstraction — platform-specific inter-process communication.
#ifndef APPS_PEERCONNECTION_ENGINE_IPC_SERVER_INTERFACE_H_
#define APPS_PEERCONNECTION_ENGINE_IPC_SERVER_INTERFACE_H_

#include <functional>
#include <string>

class IIpcServer {
 public:
  using CommandHandler = std::function<void(const std::string& json)>;

  virtual ~IIpcServer() = default;

  // Start listening on platform-specific transport.
  virtual bool Start(const std::string& path_or_address) = 0;

  // Blocking main loop — processes incoming commands.
  virtual void Run() = 0;

  // Send an event back to the connected client.
  virtual void SendEvent(const std::string& json) = 0;

  // Set handler for incoming commands.
  virtual void SetCommandHandler(CommandHandler handler) = 0;
};

#endif  // APPS_PEERCONNECTION_ENGINE_IPC_SERVER_INTERFACE_H_
