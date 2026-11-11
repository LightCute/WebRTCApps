#include "signaling_server.h"

#include <websocketpp/close.hpp>
#include <websocketpp/frame.hpp>

#include <chrono>
#include <vector>

#include "messages.h"

using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;
using websocketpp::lib::bind;

SignalingServer::SignalingServer(int port, int ping_interval_s,
                                 int ping_timeout_s, Logger& logger)
    : logger_(logger),
      port_(port),
      ping_interval_s_(ping_interval_s),
      ping_timeout_s_(ping_timeout_s) {
  endpoint_.clear_access_channels(websocketpp::log::alevel::all);
  endpoint_.clear_error_channels(websocketpp::log::elevel::all);
  endpoint_.init_asio();
  // 允许端口在 TIME_WAIT 期间立即重用，否则服务器重启后 60s 内 bind 会报 Address already in use。
  endpoint_.set_reuse_addr(true);
  endpoint_.set_open_handler(bind(&SignalingServer::OnOpen, this, ::_1));
  endpoint_.set_close_handler(bind(&SignalingServer::OnClose, this, ::_1));
  endpoint_.set_fail_handler(bind(&SignalingServer::OnFail, this, ::_1));
  endpoint_.set_message_handler(
      bind(&SignalingServer::OnMessage, this, ::_1, ::_2));
  endpoint_.set_pong_handler(bind(&SignalingServer::OnPong, this, ::_1, ::_2));
  timer_.reset(
      new websocketpp::lib::asio::steady_timer(endpoint_.get_io_service()));
}

void SignalingServer::Run() {
  endpoint_.listen(port_);
  endpoint_.start_accept();
  ScheduleHeartbeat();
  logger_.Info("listening on port " + std::to_string(port_));
  endpoint_.run();
}

void SignalingServer::Stop() {
  timer_->cancel();
  endpoint_.stop();
}

void SignalingServer::OnOpen(websocketpp::connection_hdl hdl) {
  // Track from open so half-open connections that never register are still
  // pinged and dropped by the heartbeat (spec §7).
  last_pong_[hdl] = std::time(nullptr);
  logger_.Debug("connection opened");
}

void SignalingServer::OnClose(websocketpp::connection_hdl hdl) {
  logger_.Debug("connection closed");
  DropConnection(hdl);
}

void SignalingServer::OnFail(websocketpp::connection_hdl hdl) {
  logger_.Warn("connection failed");
  DropConnection(hdl);
}

void SignalingServer::OnPong(websocketpp::connection_hdl hdl, std::string) {
  last_pong_[hdl] = std::time(nullptr);
}

void SignalingServer::OnMessage(websocketpp::connection_hdl hdl,
                                server::message_ptr msg) {
  Json::Value j;
  if (!ParseJson(msg->get_payload(), j)) {
    logger_.Warn("malformed message ignored");
    return;
  }
  std::string type = GetStringField(j, "type", "");
  auto self_it = hdl_to_id_.find(hdl);

  if (type == "register") {
    if (self_it != hdl_to_id_.end()) {
      return;  // already registered, ignore re-register
    }
    std::string name = GetStringField(j, "name", "");
    if (name.empty()) {
      return;
    }
    int id = registry_.Register(name);
    if (id < 0) {
      logger_.Warn("register rejected, name taken: " + name);
      SendToHdl(hdl, BuildError("name_taken", "name already taken"));
      return;
    }
    id_to_hdl_[id] = hdl;
    hdl_to_id_[hdl] = id;
    last_pong_[hdl] = std::time(nullptr);
    logger_.Info("client registered: " + name + " -> id " + std::to_string(id));
    SendToHdl(hdl, BuildWelcome(id));
    BroadcastRoster();
    return;
  }

  if (self_it == hdl_to_id_.end()) {
    return;  // not registered
  }
  int self = self_it->second;

  if (type == "call") {
    int to = GetIntField(j, "to", 0);
    auto r = registry_.Call(self, to);
    if (r == ClientRegistry::CallResult::kOk) {
      logger_.Info("call: " + std::to_string(self) + " -> " +
                   std::to_string(to));
      SendTo(to, StampFrom(j, self));
      BroadcastRoster();
    } else if (r == ClientRegistry::CallResult::kTargetBusy) {
      logger_.Info("call rejected (busy): " + std::to_string(self) + " -> " +
                   std::to_string(to));
      SendToHdl(hdl, BuildError("peer_busy", "peer is busy"));
    } else if (r == ClientRegistry::CallResult::kTargetOffline) {
      logger_.Info("call rejected (offline): " + std::to_string(self) +
                   " -> " + std::to_string(to));
      SendToHdl(hdl, BuildError("peer_offline", "peer is offline"));
    }
    // kCallerBusy -> ignore
  } else if (type == "answer" || type == "candidate" ||
             type == "switch_ice" || type == "reconnect" ||
             type == "reoffer") {
    int to = GetIntField(j, "to", 0);
    if (registry_.IsPaired(self, to)) {
      SendTo(to, StampFrom(j, self));
    }
  } else if (type == "message") {
    int to = GetIntField(j, "to", 0);
    std::string text = GetStringField(j, "text", "");
    if (registry_.Has(to)) {
      logger_.Info("message: " + std::to_string(self) + " -> " +
                   std::to_string(to));
      SendTo(to, BuildMessage(self, text));
    } else {
      SendToHdl(hdl, BuildError("peer_offline", "peer is offline"));
    }
  } else if (type == "leave") {
    int peer = registry_.GetPeer(self);
    if (peer != 0) {
      logger_.Info("client " + std::to_string(self) + " left call with " +
                   std::to_string(peer));
      SendToHdl(hdl, BuildLeave(self));
      SendTo(peer, BuildLeave(self));
      registry_.Hangup(self, peer);
      BroadcastRoster();
    }
  } else if (type == "get_roster") {
    logger_.Debug("get_roster from " + std::to_string(self));
    SendToHdl(hdl, BuildRoster(registry_.Roster()));
  }
  // unknown type -> ignore
}

void SignalingServer::SendToHdl(websocketpp::connection_hdl hdl,
                                const std::string& json) {
  websocketpp::lib::error_code ec;
  endpoint_.send(hdl, json, websocketpp::frame::opcode::text, ec);
}

void SignalingServer::SendTo(int id, const std::string& json) {
  auto it = id_to_hdl_.find(id);
  if (it != id_to_hdl_.end()) {
    SendToHdl(it->second, json);
  }
}

void SignalingServer::BroadcastRoster() {
  std::string roster = BuildRoster(registry_.Roster());
  for (const auto& kv : id_to_hdl_) {
    SendToHdl(kv.second, roster);
  }
}

void SignalingServer::DropConnection(websocketpp::connection_hdl hdl) {
  last_pong_.erase(hdl);
  auto it = hdl_to_id_.find(hdl);
  if (it == hdl_to_id_.end()) {
    return;  // never registered; nothing else to clean
  }
  int id = it->second;
  hdl_to_id_.erase(it);
  id_to_hdl_.erase(id);
  logger_.Info("client " + std::to_string(id) + " disconnected");
  int peer = registry_.Remove(id);
  if (peer != 0) {
    SendTo(peer, BuildPeerGone(id));
  }
  BroadcastRoster();
}

void SignalingServer::ScheduleHeartbeat() {
  timer_->expires_after(std::chrono::seconds(ping_interval_s_));
  timer_->async_wait(bind(&SignalingServer::OnHeartbeat, this, ::_1));
}

void SignalingServer::OnHeartbeat(const websocketpp::lib::error_code& ec) {
  if (ec) {
    return;  // timer cancelled (Stop)
  }
  std::time_t now = std::time(nullptr);
  std::vector<websocketpp::connection_hdl> to_drop;
  for (const auto& kv : last_pong_) {
    websocketpp::lib::error_code ignored;
    endpoint_.ping(kv.first, "", ignored);
    if ((now - kv.second) > ping_timeout_s_) {
      to_drop.push_back(kv.first);
    }
  }
  if (!to_drop.empty()) {
    logger_.Warn("ping timeout, dropping " + std::to_string(to_drop.size()) +
                 " connection(s)");
  }
  for (const auto& hdl : to_drop) {
    websocketpp::lib::error_code ignored;
    endpoint_.close(hdl, websocketpp::close::status::normal, "ping timeout",
                    ignored);
  }
  ScheduleHeartbeat();
}
