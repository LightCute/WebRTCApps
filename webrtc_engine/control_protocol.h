// control_protocol.h — JSON-RPC command parser, transport-agnostic.
#ifndef APPS_WEBRTC_ENGINE_CONTROL_PROTOCOL_H_
#define APPS_WEBRTC_ENGINE_CONTROL_PROTOCOL_H_

#include <functional>
#include <string>

class EngineController;
class IPipeTransport;

class ControlProtocol {
 public:
  ControlProtocol(IPipeTransport* transport, EngineController* engine);

  // Called by transport when a complete line arrives.
  void OnLineReceived(const std::string& line);

  // Helper for sending JSON events back to client.
  void SendEvent(const std::string& json);
  void SendResponse(int id, bool ok, const std::string& error = "");

  // Optional callback for system commands (shutdown).
  std::function<void()> on_shutdown;

 private:
  void HandleCommand(const std::string& raw_json);

  IPipeTransport* transport_;
  EngineController* engine_;
};

#endif  // APPS_WEBRTC_ENGINE_CONTROL_PROTOCOL_H_
