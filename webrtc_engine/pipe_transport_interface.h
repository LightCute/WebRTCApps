// pipe_transport_interface.h — cross-platform byte-stream IPC abstraction.
//
// Implementations:
//   Linux   — UnixSocketTransport  (AF_UNIX)
//   Windows — NamedPipeTransport   (CreateNamedPipe)
//
// Consumer: ProtocolHandler — parses JSON lines, dispatches commands.
#ifndef APPS_WEBRTC_ENGINE_PIPE_TRANSPORT_INTERFACE_H_
#define APPS_WEBRTC_ENGINE_PIPE_TRANSPORT_INTERFACE_H_

#include <functional>
#include <string>

class IPipeTransport {
 public:
  virtual ~IPipeTransport() = default;

  // Start listening on platform-specific endpoint.
  virtual bool Start(const std::string& endpoint) = 0;

  // Block until shutdown. The transport internally pumps I/O and
  // calls the upper-layer callbacks.
  virtual void Run() = 0;

  // Send data to the connected client (no framing needed).
  virtual void Send(const std::string& data) = 0;

  // Stop and clean up.
  virtual void Stop() = 0;

  // Callbacks — called from the I/O thread when data arrives or
  // the client connects/disconnects.
  std::function<void(const std::string& line)> on_message;
  std::function<void()> on_connected;
  std::function<void()> on_disconnected;
};

#endif  // APPS_WEBRTC_ENGINE_PIPE_TRANSPORT_INTERFACE_H_
