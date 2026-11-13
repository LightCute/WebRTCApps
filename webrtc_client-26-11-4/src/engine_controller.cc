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
    "set_ice_transports <mode>",   "switch_ice <mode>",
    "reconnect",
    "get_ice_config",
    "start_video",                  "stop_video",
    "video_on",                     "video_off",
    "set_bitrate <kbps>",           "set_framerate <fps>",
    "set_degradation <mode>",       "get_video_status",
    "get_video_devices",            "set_video_device <id>",
    "get_video_device_capabilities <id>",
    "set_video_profile <w> <h> <fps>",
    "set_video_stats <on|off> [interval_ms]", "request_keyframe",
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

const char* ModeToString(IceTransportsMode m) {
  switch (m) {
    case IceTransportsMode::kHost: return "host";
    case IceTransportsMode::kSrflx: return "srflx";
    case IceTransportsMode::kRelay: return "relay";
    case IceTransportsMode::kAuto: return "auto";
  }
  return "auto";
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

  Json::Value lcs(Json::arrayValue);
  for (const auto& c : stats.local_candidates) {
    Json::Value e;
    e["ip"] = c.ip;
    e["port"] = c.port;
    e["protocol"] = c.protocol;
    e["candidate_type"] = c.candidate_type;
    e["selected"] = c.selected;
    lcs.append(e);
  }
  v["local_candidates"] = lcs;

  Json::Value rcs(Json::arrayValue);
  for (const auto& c : stats.remote_candidates) {
    Json::Value e;
    e["ip"] = c.ip;
    e["port"] = c.port;
    e["protocol"] = c.protocol;
    e["candidate_type"] = c.candidate_type;
    e["selected"] = c.selected;
    rcs.append(e);
  }
  v["remote_candidates"] = rcs;

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

void EngineController::OnSignalSwitchIce(int from) {
  Json::Value v = Event("signal_client.switch_ice");
  v["from"] = from;
  Send(SerializeJson(v));
}

void EngineController::OnSignalReconnect(int from) {
  Json::Value v = Event("signal_client.reconnect");
  v["from"] = from;
  Send(SerializeJson(v));
}

void EngineController::OnIceSwitched(bool success) {
  Json::Value v = Event("ice_switched");
  v["result"] = success ? "success" : "failed";
  Send(SerializeJson(v));
}

void EngineController::OnReconnected(bool success) {
  Json::Value v = Event("reconnected");
  v["result"] = success ? "success" : "failed";
  Send(SerializeJson(v));
}

void EngineController::OnVideoStateChanged(bool enabled) {
  Json::Value v = Event("video.state");
  v["enabled"] = enabled;
  Send(SerializeJson(v));
}

void EngineController::OnVideoSwitched(bool enabled, bool success) {
  Json::Value v = Event("video_switched");
  v["enabled"] = enabled;
  v["result"] = success ? "success" : "failed";
  Send(SerializeJson(v));
}

void EngineController::OnVideoState(const VideoState& state) {
  Json::Value v = Event("video_params");
  // 开关 / 通话状态
  v["enabled"] = state.enabled;
  v["in_call"] = state.in_call;
  // 当前设备
  v["device_id"] = state.device_id;
  v["device_name"] = state.device_name;
  v["device_synthetic"] = state.device_synthetic;
  // 采集档位 + 实际生效格式
  v["capture_width"] = state.capture_width;
  v["capture_height"] = state.capture_height;
  v["capture_fps"] = state.capture_fps;
  v["capture_format"] = state.capture_format;
  // 编码参数
  v["width"] = state.encode_width;
  v["height"] = state.encode_height;
  v["fps"] = state.encode_fps;
  v["bitrate_kbps"] = state.encode_bitrate_kbps;
  v["degradation"] = state.encode_degradation;
  Send(SerializeJson(v));
}

void EngineController::OnVideoStats(const VideoStats& stats) {
  Json::Value v = Event("video_stats");
  v["width"] = stats.width;
  v["height"] = stats.height;
  v["fps"] = stats.fps;
  v["target_bitrate"] = stats.target_bitrate;
  v["bytes_sent"] = static_cast<Json::UInt64>(stats.bytes_sent);
  v["packets_sent"] = static_cast<Json::UInt>(stats.packets_sent);
  v["packets_lost"] = stats.packets_lost;
  v["jitter_ms"] = stats.jitter_ms;
  v["rtt_ms"] = stats.rtt_ms;
  v["available_outgoing_bitrate"] = stats.available_outgoing_bitrate;
  v["quality_limitation"] = stats.quality_limitation;
  v["frames_encoded"] = static_cast<Json::UInt>(stats.frames_encoded);
  v["key_frames_encoded"] = static_cast<Json::UInt>(stats.key_frames_encoded);
  v["pli_count"] = static_cast<Json::UInt>(stats.pli_count);
  v["nack_count"] = static_cast<Json::UInt>(stats.nack_count);
  Send(SerializeJson(v));
}

void EngineController::OnKeyFrameRequested(bool success) {
  Json::Value v = Event("keyframe_requested");
  v["result"] = success ? "success" : "failed";
  Send(SerializeJson(v));
}

void EngineController::OnVideoDevices(
    const std::vector<webrtc_media::VideoDeviceInfo>& devices,
    const std::string& current_id) {
  Json::Value v = Event("video_devices");
  v["current"] = current_id;
  Json::Value arr(Json::arrayValue);
  for (const auto& d : devices) {
    Json::Value e;
    e["id"] = d.id;
    e["name"] = d.name;
    e["synthetic"] = d.is_synthetic;
    e["width"] = d.width;
    e["height"] = d.height;
    e["fps"] = d.fps;
    arr.append(e);
  }
  v["devices"] = arr;
  Send(SerializeJson(v));
}

void EngineController::OnVideoDeviceChanged(const std::string& id,
                                            const std::string& name,
                                            bool success) {
  Json::Value v = Event("video_device");
  v["id"] = id;
  v["name"] = name;
  v["result"] = success ? "success" : "failed";
  Send(SerializeJson(v));
}

void EngineController::OnVideoDeviceCapabilities(
    const std::string& id,
    const std::vector<webrtc_media::VideoFormatCapability>& formats) {
  Json::Value v = Event("video_device_capabilities");
  v["id"] = id;
  Json::Value arr(Json::arrayValue);
  for (const auto& fmt : formats) {
    Json::Value f;
    f["pixel_format"] = fmt.pixel_format;
    Json::Value sizes(Json::arrayValue);
    for (const auto& size : fmt.sizes) {
      Json::Value s;
      s["width"] = size.width;
      s["height"] = size.height;
      Json::Value fps(Json::arrayValue);
      for (int f2 : size.fps) {
        fps.append(f2);
      }
      s["fps"] = fps;
      // 只有「该尺寸实际会用的格式」才可选（与 set_video_profile 的校验一致）。
      s["selectable"] =
          (fmt.pixel_format == webrtc_media::EffectivePixelFormat(
                                    formats, size.width, size.height));
      sizes.append(s);
    }
    f["sizes"] = sizes;
    arr.append(f);
  }
  v["formats"] = arr;
  Send(SerializeJson(v));
}

void EngineController::OnVideoProfile(int width, int height, int fps,
                                      bool success) {
  Json::Value v = Event("video_profile");
  v["width"] = width;
  v["height"] = height;
  v["fps"] = fps;
  v["result"] = success ? "success" : "failed";
  Send(SerializeJson(v));
}

void EngineController::OnVideoDevicesChanged(
    const std::vector<webrtc_media::VideoDeviceInfo>& devices,
    const std::string& current_id, const std::string& reason) {
  Json::Value v = Event("video_devices_changed");
  v["current"] = current_id;
  v["reason"] = reason;
  Json::Value arr(Json::arrayValue);
  for (const auto& d : devices) {
    Json::Value e;
    e["id"] = d.id;
    e["name"] = d.name;
    e["synthetic"] = d.is_synthetic;
    e["width"] = d.width;
    e["height"] = d.height;
    e["fps"] = d.fps;
    arr.append(e);
  }
  v["devices"] = arr;
  Send(SerializeJson(v));
}

void EngineController::OnVideoDeviceLost(const std::string& id) {
  Json::Value v = Event("video_device_lost");
  v["id"] = id;
  Send(SerializeJson(v));
}

void EngineController::OnRemoteVideoStarted() {
  Send(SerializeJson(Event("remote_video.started")));
}

void EngineController::OnRemoteVideoStopped() {
  Send(SerializeJson(Event("remote_video.stopped")));
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
    // Layer 1：自呼校验（呼叫自己的 id，不建 PC/SDP）。
    if (peer == engine_->my_id()) {
      SendError("self_call", "cannot call yourself");
      return;
    }
    // Layer 1：roster 预校验（peers_ 由服务器每次广播刷新），离线和忙线分开。
    bool found = false;
    bool idle = false;
    for (const auto& p : peers_) {
      if (p.id == peer) {
        found = true;
        idle = (p.state == "idle");
        break;
      }
    }
    if (!found) {
      SendError("peer_offline", "peer is offline");
      return;
    }
    if (!idle) {
      SendError("peer_busy", "peer is busy");
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
  } else if (cmd == "set_ice_transports") {
    std::string mode;
    ss >> mode;
    if (mode.empty()) {
      SendError("usage", "usage: set_ice_transports <mode>");
      return;
    }
    if (engine_->is_connected() || engine_->peer_id() > 0) {
      SendError("already_in_call", "already in a call");
      return;
    }
    engine_->SetIceTransports(mode);
    SendIceConfig();
  } else if (cmd == "switch_ice") {
    std::string mode;
    ss >> mode;
    if (mode.empty()) {
      SendError("usage", "usage: switch_ice <mode>");
      return;
    }
    if (!engine_->is_connected() && engine_->peer_id() <= 0) {
      SendError("not_in_call", "not in a call");
      return;
    }
    engine_->SwitchIce(mode);
    SendIceConfig();
  } else if (cmd == "reconnect") {
    if (!engine_->is_connected() && engine_->peer_id() <= 0) {
      SendError("not_in_call", "not in a call");
      return;
    }
    engine_->Reconnect();
  } else if (cmd == "start_video") {
    if (engine_->is_connected() || engine_->peer_id() > 0) {
      SendError("already_in_call", "already in a call");
      return;
    }
    engine_->StartVideo();
  } else if (cmd == "stop_video") {
    if (engine_->is_connected() || engine_->peer_id() > 0) {
      SendError("already_in_call", "already in a call");
      return;
    }
    engine_->StopVideo();
  } else if (cmd == "video_on") {
    if (!engine_->is_connected() && engine_->peer_id() <= 0) {
      SendError("not_in_call", "not in a call");
      return;
    }
    engine_->VideoOn();
  } else if (cmd == "video_off") {
    if (!engine_->is_connected() && engine_->peer_id() <= 0) {
      SendError("not_in_call", "not in a call");
      return;
    }
    engine_->VideoOff();
  } else if (cmd == "set_bitrate") {
    int kbps = 0;
    ss >> kbps;
    if (kbps <= 0) {
      SendError("usage", "usage: set_bitrate <kbps>");
      return;
    }
    if (!engine_->is_connected() && engine_->peer_id() <= 0) {
      SendError("not_in_call", "not in a call");
      return;
    }
    engine_->SetBitrate(kbps);
  } else if (cmd == "set_framerate") {
    int fps = 0;
    ss >> fps;
    if (fps <= 0) {
      SendError("usage", "usage: set_framerate <fps>");
      return;
    }
    if (!engine_->is_connected() && engine_->peer_id() <= 0) {
      SendError("not_in_call", "not in a call");
      return;
    }
    engine_->SetFramerate(fps);
  } else if (cmd == "set_degradation") {
    std::string mode;
    ss >> mode;
    if (mode.empty()) {
      SendError("usage", "usage: set_degradation <mode>");
      return;
    }
    if (!engine_->is_connected() && engine_->peer_id() <= 0) {
      SendError("not_in_call", "not in a call");
      return;
    }
    engine_->SetDegradation(mode);
  } else if (cmd == "get_video_status") {
    engine_->GetVideoStatus();
  } else if (cmd == "get_video_devices") {
    engine_->GetVideoDevices();
  } else if (cmd == "set_video_device") {
    std::string id;
    ss >> id;
    if (id.empty()) {
      SendError("usage", "usage: set_video_device <id>");
      return;
    }
    if (!engine_->SetVideoDevice(id)) {
      SendError("device_not_found", "video device not found: " + id);
    }
  } else if (cmd == "get_video_device_capabilities") {
    std::string id;
    ss >> id;
    if (id.empty()) {
      SendError("usage", "usage: get_video_device_capabilities <id>");
      return;
    }
    if (!engine_->GetVideoDeviceCapabilities(id)) {
      SendError("device_not_found", "video device not found: " + id);
    }
  } else if (cmd == "set_video_profile") {
    int w = 0, h = 0, fps = 0;
    ss >> w >> h >> fps;
    if (w <= 0 || h <= 0 || fps <= 0) {
      SendError("usage", "usage: set_video_profile <w> <h> <fps>");
      return;
    }
    if (!engine_->SetVideoProfile(w, h, fps)) {
      SendError("unsupported_profile",
                "video device does not support " + std::to_string(w) + "x" +
                    std::to_string(h) + "@" + std::to_string(fps));
    }
  } else if (cmd == "set_video_stats") {
    std::string mode;
    ss >> mode;
    if (mode != "on" && mode != "off") {
      SendError("usage", "usage: set_video_stats <on|off> [interval_ms]");
      return;
    }
    int interval_ms = 0;
    ss >> interval_ms;
    engine_->SetVideoStats(mode == "on", interval_ms);
  } else if (cmd == "request_keyframe") {
    if (!engine_->is_connected() && engine_->peer_id() <= 0) {
      SendError("not_in_call", "not in a call");
      return;
    }
    engine_->RequestKeyFrame();
  } else if (cmd == "get_ice_config") {
    SendIceConfig();
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

void EngineController::SendIceConfig() {
  const IceConfig& cfg = engine_->ice_config();
  Json::Value v = Event("ice_config");
  v["mode"] = ModeToString(cfg.mode);
  v["stun"] = cfg.stun_uri;
  v["turn"] = cfg.turn_uri;
  v["turn_user"] = cfg.turn_user;
  v["turn_pass"] = cfg.turn_pass;
  Send(SerializeJson(v));
}

void EngineController::Send(const std::string& msg) {
  if (client_id_ >= 0) {
    server_->SendToClient(client_id_, msg);
  }
}
