// control_protocol.h — JSON-RPC command parser, transport-agnostic.
#ifndef APPS_WEBRTC_ENGINE_CONTROL_PROTOCOL_H_
#define APPS_WEBRTC_ENGINE_CONTROL_PROTOCOL_H_

#include <functional>
#include <string>

#include "apps/webrtc_engine/engine_controller.h"

class IPipeTransport;

class ControlProtocol : public EngineObserver {
 public:
  ControlProtocol(IPipeTransport* transport, EngineController* engine);

  // Called by transport when a complete line arrives.
  void OnLineReceived(const std::string& line);

  // EngineObserver — engine events forwarded to transport.
  void OnEngineEvent(const std::string& json) override;

  // Helper for sending JSON responses back to client.
  void SendResponse(int id, bool ok, const std::string& error = "");

  // Optional callback for system commands (shutdown).
  std::function<void()> on_shutdown;

 private:
  void HandleCommand(const std::string& raw_json);

  IPipeTransport* transport_;
  EngineController* engine_;
};

#endif  // APPS_WEBRTC_ENGINE_CONTROL_PROTOCOL_H_
