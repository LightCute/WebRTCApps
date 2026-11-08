#include "engine_controller.h"

#include <functional>
#include <sstream>

#include <json/json.h>

#include "api/data_channel_interface.h"
#include "api/peer_connection_interface.h"
#include "rtc_base/thread.h"
#include "webrtc_engine.h"

EngineController::EngineController(webrtc_engine* engine, UnixSocketServer* server,
                     const std::string& signal_server, int signal_port)
    : engine_(engine),
      server_(server),
      signal_server_(signal_server),
      signal_port_(signal_port) {}

namespace {

// 后台进程支持的交互命令（上层连接成功时一次性展示）。
const char* const kCommands[] = {
    "register <name> [ip] [port]", "call <peer_id>",
    "data_channel_msg <text>",     "signal_msg <peer_id> <text>",
    "leave",                       "get_roster",
    "peer_connection_status",      "signal_disconnect",
    "get_stats",                   "quit",
};

// 单行紧凑 JSON（indentation 置空），保证不破坏 unix socket 的行分帧。
std::string SerializeJson(const Json::Value& v) {
  Json::StreamWriterBuilder b;
  b["indentation"] = "";
  std::unique_ptr<Json::StreamWriter> w(b.newStreamWriter());
  std::ostringstream os;
  w->write(v, &os);
  return os.str();
}

Json::Value Event(const std::string& name) {
  Json::Value v;
  v["event"] = name;
  return v;
}

}  // namespace

// ===== UnixSocketObserver（io 线程，快速返回）=====

void EngineController::OnClientConnected(int client_id) {
  client_id_ = client_id;
  Json::Value v = Event("ipc.connected");
  Json::Value cmds(Json::arrayValue);
  for (const char* c : kCommands) {
    cmds.append(c);
  }
  v["commands"] = cmds;
  Send(SerializeJson(v));
}

void EngineController::OnLineReceived(int client_id, const std::string& line) {
  client_id_ = client_id;
  webrtc::Thread* st = engine_->signaling_thread();
  if (st) {
    // 命令在 io 线程，投递到 signaling 线程执行，避免阻塞 io 线程。
    std::function<void()> fn = [this, line] { HandleCommand(line); };
    st->PostTask(std::move(fn));
  }
}

void EngineController::OnClientDisconnected(int client_id) {
  client_id_ = -1;
}

// ===== EngineObserver（signaling 线程）=====

void EngineController::OnSignalSignedIn(int my_id) {
  Json::Value v = Event("signal_client.signed_in");
  v["id"] = my_id;
  Send(SerializeJson(v));
}

void EngineController::OnSignalDisconnected() {
  Send(SerializeJson(Event("signal_client.disconnected")));
}

void EngineController::OnSignalRoster(const std::vector<PeerInfo>& peers) {
  peers_ = peers;
  Json::Value v = Event("signal_client.roster");
  Json::Value arr(Json::arrayValue);
  for (const auto& p : peers) {
    Json::Value e;
    e["id"] = p.id;
    e["name"] = p.name;
    e["state"] = p.state;  // "idle" | "busy"
    arr.append(e);
  }
  v["peers"] = arr;
  Send(SerializeJson(v));
}

void EngineController::OnSignalCall(int from) {
  Json::Value v = Event("signal_client.incoming_call");
  v["from"] = from;
  Send(SerializeJson(v));
}

void EngineController::OnSignalMessage(int from, const std::string& text) {
  Json::Value v = Event("signal_client.message");
  v["from"] = from;
  v["text"] = text;
  Send(SerializeJson(v));
}

void EngineController::OnCallEnded(int from) {
  Json::Value v = Event("signal_client.call_ended");
  v["from"] = from;
  Send(SerializeJson(v));
}

void EngineController::OnSignalError(const std::string& code,
                                     const std::string& msg) {
  Json::Value v = Event("signal_client.error");
  v["code"] = code;
  v["msg"] = msg;
  Send(SerializeJson(v));
}

void EngineController::OnEngineError(const std::string& code,
                                     const std::string& msg) {
  Json::Value v = Event("engine.error");
  v["code"] = code;
  v["msg"] = msg;
  Send(SerializeJson(v));
}

void EngineController::OnLocalSdp(const std::string& type, const std::string& sdp) {
  Json::Value v = Event("signal_client.local_sdp");
  v["type"] = type;
  v["sdp"] = sdp;  // jsoncpp 自动转义换行，保持单行
  Send(SerializeJson(v));
}

void EngineController::OnRemoteSdp(const std::string& type, const std::string& sdp) {
  Json::Value v = Event("signal_client.remote_sdp");
  v["type"] = type;
  v["sdp"] = sdp;
  Send(SerializeJson(v));
}

void EngineController::OnLocalCandidate(const std::string& candidate,
                                 const std::string& sdp_mid,
                                 int sdp_mline_index) {
  Json::Value v = Event("signal_client.local_candidate");
  v["candidate"] = candidate;
  v["mid"] = sdp_mid;
  v["index"] = sdp_mline_index;
  Send(SerializeJson(v));
}

void EngineController::OnRemoteCandidate(const std::string& candidate,
                                  const std::string& sdp_mid,
                                  int sdp_mline_index) {
  Json::Value v = Event("signal_client.remote_candidate");
  v["candidate"] = candidate;
  v["mid"] = sdp_mid;
  v["index"] = sdp_mline_index;
  Send(SerializeJson(v));
}

void EngineController::OnPeerConnectionStateChange(
    webrtc::PeerConnectionInterface::PeerConnectionState state) {
  Json::Value v = Event("peer_connection.state");
  v["state"] = std::string(webrtc::PeerConnectionInterface::AsString(state));
  Send(SerializeJson(v));
}

void EngineController::OnDataChannelStateChange(
    webrtc::DataChannelInterface::DataState state) {
  Json::Value v = Event("data_channel.state");
  v["state"] =
      std::string(webrtc::DataChannelInterface::DataStateString(state));
  Send(SerializeJson(v));
}

void EngineController::OnDataChannelMessage(const std::string& message) {
  Json::Value v = Event("data_channel_msg");
  v["text"] = message;
  Send(SerializeJson(v));
}

void EngineController::OnStats(const EngineStats& stats) {
  Json::Value v = Event("stats");

  Json::Value dc;
  dc["state"] = stats.data_channel.state;
  dc["label"] = stats.data_channel.label;
  dc["protocol"] = stats.data_channel.protocol;
  dc["reliable"] = stats.data_channel.reliable;
  dc["ordered"] = stats.data_channel.ordered;
  dc["id"] = stats.data_channel.id;
  dc["messages_sent"] = static_cast<Json::UInt>(stats.data_channel.messages_sent);
  dc["bytes_sent"] = static_cast<Json::UInt64>(stats.data_channel.bytes_sent);
  dc["messages_received"] =
      static_cast<Json::UInt>(stats.data_channel.messages_received);
  dc["bytes_received"] =
      static_cast<Json::UInt64>(stats.data_channel.bytes_received);
  dc["buffered_amount"] =
      static_cast<Json::UInt64>(stats.data_channel.buffered_amount);
  v["data_channel"] = dc;

  Json::Value tr;
  tr["bytes_sent"] = static_cast<Json::UInt64>(stats.transport.bytes_sent);
  tr["packets_sent"] = static_cast<Json::UInt64>(stats.transport.packets_sent);
  tr["bytes_received"] =
      static_cast<Json::UInt64>(stats.transport.bytes_received);
  tr["packets_received"] =
      static_cast<Json::UInt64>(stats.transport.packets_received);
  tr["dtls_state"] = stats.transport.dtls_state;
  tr["ice_state"] = stats.transport.ice_state;
  tr["ice_role"] = stats.transport.ice_role;
  tr["selected_candidate_pair_id"] = stats.transport.selected_candidate_pair_id;
  v["transport"] = tr;

  Json::Value cp;
  cp["state"] = stats.candidate_pair.state;
  cp["nominated"] = stats.candidate_pair.nominated;
  cp["writable"] = stats.candidate_pair.writable;
  cp["packets_sent"] =
      static_cast<Json::UInt64>(stats.candidate_pair.packets_sent);
  cp["packets_received"] =
      static_cast<Json::UInt64>(stats.candidate_pair.packets_received);
  cp["bytes_sent"] = static_cast<Json::UInt64>(stats.candidate_pair.bytes_sent);
  cp["bytes_received"] =
      static_cast<Json::UInt64>(stats.candidate_pair.bytes_received);
  cp["current_round_trip_time"] = stats.candidate_pair.current_round_trip_time;
  cp["total_round_trip_time"] = stats.candidate_pair.total_round_trip_time;
  cp["available_outgoing_bitrate"] =
      stats.candidate_pair.available_outgoing_bitrate;
  cp["available_incoming_bitrate"] =
      stats.candidate_pair.available_incoming_bitrate;

  Json::Value lc;
  lc["ip"] = stats.candidate_pair.local_candidate.ip;
  lc["port"] = stats.candidate_pair.local_candidate.port;
  lc["protocol"] = stats.candidate_pair.local_candidate.protocol;
  lc["candidate_type"] = stats.candidate_pair.local_candidate.candidate_type;
  cp["local_candidate"] = lc;

  Json::Value rc;
  rc["ip"] = stats.candidate_pair.remote_candidate.ip;
  rc["port"] = stats.candidate_pair.remote_candidate.port;
  rc["protocol"] = stats.candidate_pair.remote_candidate.protocol;
  rc["candidate_type"] = stats.candidate_pair.remote_candidate.candidate_type;
  cp["remote_candidate"] = rc;

  v["candidate_pair"] = cp;

  v["data_channels_opened"] = stats.data_channels_opened;
  v["data_channels_closed"] = stats.data_channels_closed;

  Json::Value in(Json::arrayValue);
  for (const auto& s : stats.inbound_rtp) {
    Json::Value e;
    e["kind"] = s.kind;
    e["codec"] = s.codec;
    e["ssrc"] = static_cast<Json::UInt>(s.ssrc);
    e["packets_received"] = static_cast<Json::UInt>(s.packets_received);
    e["bytes_received"] = static_cast<Json::UInt64>(s.bytes_received);
    e["packets_lost"] = static_cast<Json::Int>(s.packets_lost);
    e["jitter"] = s.jitter;
    e["frames_per_second"] = s.frames_per_second;
    e["frame_width"] = static_cast<Json::UInt>(s.frame_width);
    e["frame_height"] = static_cast<Json::UInt>(s.frame_height);
    e["frames_decoded"] = static_cast<Json::UInt>(s.frames_decoded);
    e["frames_dropped"] = static_cast<Json::UInt>(s.frames_dropped);
    e["audio_level"] = s.audio_level;
    in.append(e);
  }
  v["inbound_rtp"] = in;

  Json::Value out(Json::arrayValue);
  for (const auto& s : stats.outbound_rtp) {
    Json::Value e;
    e["kind"] = s.kind;
    e["codec"] = s.codec;
    e["ssrc"] = static_cast<Json::UInt>(s.ssrc);
    e["packets_sent"] = static_cast<Json::UInt>(s.packets_sent);
    e["bytes_sent"] = static_cast<Json::UInt64>(s.bytes_sent);
    e["target_bitrate"] = s.target_bitrate;
    e["frames_encoded"] = static_cast<Json::UInt>(s.frames_encoded);
    e["frames_per_second"] = s.frames_per_second;
    e["frame_width"] = static_cast<Json::UInt>(s.frame_width);
    e["frame_height"] = static_cast<Json::UInt>(s.frame_height);
    e["quality_limitation_reason"] = s.quality_limitation_reason;
    out.append(e);
  }
  v["outbound_rtp"] = out;

  Json::Value rin(Json::arrayValue);
  for (const auto& s : stats.remote_inbound_rtp) {
    Json::Value e;
    e["kind"] = s.kind;
    e["ssrc"] = static_cast<Json::UInt>(s.ssrc);
    e["round_trip_time"] = s.round_trip_time;
    e["fraction_lost"] = s.fraction_lost;
    e["jitter"] = s.jitter;
    rin.append(e);
  }
  v["remote_inbound_rtp"] = rin;

  Send(SerializeJson(v));
}

// ===== 命令处理（signaling 线程）=====

void EngineController::HandleCommand(const std::string& line) {
  std::istringstream ss(line);
  std::string cmd;
  ss >> cmd;

  if (cmd == "register") {
    std::string name;
    ss >> name;
    if (name.empty()) {
      SendError("usage", "usage: register <name> [ip] [port]");
      return;
    }
    std::string server = signal_server_;
    int port = signal_port_;
    std::string ip;
    if (ss >> ip) {
      server = ip;
      int p = 0;
      if (ss >> p) {
        port = p;
      }
    }
    engine_->ConnectToServer(server, port, name);
  } else if (cmd == "signal_disconnect") {
    if (!engine_->is_signed_in()) {
      SendError("not_signed_in", "not signed in");
      return;
    }
    engine_->DisconnectSignal();
  } else if (cmd == "call") {
    int peer = 0;
    ss >> peer;
    if (peer <= 0) {
      SendError("usage", "usage: call <peer_id>");
      return;
    }
    if (!engine_->is_signed_in()) {
      SendError("not_signed_in", "not signed in");
      return;
    }
    if (engine_->is_connected() || engine_->peer_id() > 0) {
      SendError("already_in_call", "already in a call");
      return;
    }
    engine_->StartCall(peer);
  } else if (cmd == "data_channel_msg") {
    std::string text;
    std::getline(ss, text);
    if (!text.empty() && text[0] == ' ') {
      text = text.substr(1);
    }
    if (!engine_->SendDataChannelMessage(text)) {
      SendError("data_channel_not_open", "data channel not open");
    }
  } else if (cmd == "signal_msg") {
    int peer = 0;
    ss >> peer;
    std::string text;
    std::getline(ss, text);
    if (!text.empty() && text[0] == ' ') {
      text = text.substr(1);
    }
    if (peer <= 0) {
      SendError("usage", "usage: signal_msg <peer_id> <text>");
      return;
    }
    if (!engine_->SendSignalMessage(peer, text)) {
      SendError("signal_send_failed", "failed to send signal message");
    }
  } else if (cmd == "leave") {
    if (!engine_->is_signed_in()) {
      SendError("not_signed_in", "not signed in");
      return;
    }
    if (engine_->peer_id() <= 0 && !engine_->is_connected()) {
      SendError("not_in_call", "not in a call");
      return;
    }
    engine_->Leave();
  } else if (cmd == "get_roster") {
    if (!engine_->is_signed_in()) {
      SendError("not_signed_in", "not signed in");
      return;
    }
    engine_->GetRoster();
  } else if (cmd == "get_stats") {
    engine_->RequestStats();
  } else if (cmd == "peer_connection_status") {
    Json::Value v = Event("peer_connection_status");
    v["signed_in"] = engine_->is_signed_in();
    v["my_id"] = engine_->my_id();
    v["connected"] = engine_->is_connected();
    v["data_channel_open"] = engine_->is_data_channel_open();
    v["peer_id"] = engine_->peer_id();
    Send(SerializeJson(v));
  } else if (cmd == "quit") {
    // 只发退出信号让 main 去 Stop（main 的 Wait 返回后负责停引擎）。
    // 不能在此调 engine_->Stop()：当前在 signaling 线程，Stop 会 BlockingCall 自身 → abort。
    server_->RequestShutdown();
  } else {
    SendError("usage", "unknown command: " + cmd);
  }
}

void EngineController::SendError(const std::string& code, const std::string& msg) {
  Json::Value v = Event("ipc.error");
  v["code"] = code;
  v["msg"] = msg;
  Send(SerializeJson(v));
}

void EngineController::Send(const std::string& msg) {
  if (client_id_ >= 0) {
    server_->SendToClient(client_id_, msg);
  }
}
