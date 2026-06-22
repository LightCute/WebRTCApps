// protocol_handler.cc
#include "apps/webrtc_engine/control_protocol.h"

#include <sstream>

#include "apps/webrtc_engine/engine_controller.h"
#include "apps/webrtc_engine/pipe_transport_interface.h"
#include "json/reader.h"
#include "json/value.h"
#include "json/writer.h"
#include "rtc_base/logging.h"

ControlProtocol::ControlProtocol(IPipeTransport* transport,
                                 EngineController* engine)
    : transport_(transport), engine_(engine) {}

void ControlProtocol::OnLineReceived(const std::string& line) {
  HandleCommand(line);
}

void ControlProtocol::OnEngineEvent(const std::string& json) {
  transport_->Send(json);
}

void ControlProtocol::SendResponse(int id, bool ok, const std::string& error) {
  Json::Value resp;
  resp["id"] = id;
  resp["ok"] = ok;
  if (!error.empty()) resp["error"] = error;
  Json::StreamWriterBuilder factory;
  factory["indentation"] = "";
  transport_->Send(Json::writeString(factory, resp));
}

void ControlProtocol::HandleCommand(const std::string& raw_json) {
  Json::Value root;
  Json::CharReaderBuilder factory;
  std::string errors;
  std::istringstream stream(raw_json);
  if (!Json::parseFromStream(factory, stream, &root, &errors)) {
    RTC_LOG(LS_WARNING) << "Failed to parse JSON command: " << errors;
    return;
  }

  std::string cmd = root.get("cmd", "").asString();
  int id = root.get("id", -1).asInt();

  if (cmd == "connect") {
    std::string server = root["params"].get("server", "").asString();
    int port = root["params"].get("port", 0).asInt();
    if (server.empty() || port == 0) {
      SendResponse(id, false, "Missing server or port");
      return;
    }
    engine_->ConnectToServer(server, port);
    SendResponse(id, true);
  } else if (cmd == "disconnect") {
    engine_->DisconnectFromServer();
    SendResponse(id, true);
  } else if (cmd == "call") {
    int peer_id = root["params"].get("peer_id", -1).asInt();
    if (peer_id < 0) {
      SendResponse(id, false, "Invalid peer_id");
      return;
    }
    engine_->ConnectToPeer(peer_id);
    SendResponse(id, true);
  } else if (cmd == "hangup") {
    engine_->HangUp();
    SendResponse(id, true);
  } else if (cmd == "send_data") {
    std::string text = root["params"].get("text", "").asString();
    if (text.empty()) {
      SendResponse(id, false, "Missing text in params");
      return;
    }
    engine_->SendData(text);
    SendResponse(id, true);
  } else if (cmd == "shutdown") {
    SendResponse(id, true);
    if (on_shutdown) on_shutdown();
  } else if (cmd == "get_local_sdp") {
    engine_->GetLocalSdp();
  } else if (cmd == "dump_stats") {
    engine_->DumpStats();
  } else if (cmd == "start_stats") {
    engine_->StartStatsPolling();
    SendResponse(id, true);
  } else if (cmd == "stop_stats") {
    engine_->StopStatsPolling();
    SendResponse(id, true);
  } else {
    SendResponse(id, false, "Unknown command: " + cmd);
  }
}
