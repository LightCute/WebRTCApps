#include "ws_signaling_client.h"

#include <websocketpp/close.hpp>
#include <websocketpp/frame.hpp>

#include <chrono>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

#include <json/json.h>

using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;
using websocketpp::lib::bind;

namespace {

bool ParseJson(const std::string& s, Json::Value* v) {
  Json::CharReaderBuilder b;
  std::unique_ptr<Json::CharReader> r(b.newCharReader());
  std::string errs;
  return r->parse(s.data(), s.data() + s.size(), v, &errs);
}

std::string SerializeJson(const Json::Value& v) {
  Json::StreamWriterBuilder b;
  b["indentation"] = "";
  std::unique_ptr<Json::StreamWriter> w(b.newStreamWriter());
  std::ostringstream os;
  w->write(v, &os);
  return os.str();
}

std::string GetStringField(const Json::Value& v, const char* key) {
  return (v.isObject() && v.isMember(key) && v[key].isString())
             ? v[key].asString()
             : "";
}

int GetIntField(const Json::Value& v, const char* key, int def) {
  return (v.isObject() && v.isMember(key) && v[key].isInt()) ? v[key].asInt()
                                                             : def;
}

}  // namespace

WebSocketSignalingClient::WebSocketSignalingClient() {
  endpoint_.clear_access_channels(websocketpp::log::alevel::all);
  endpoint_.clear_error_channels(websocketpp::log::elevel::all);
  endpoint_.init_asio();
  // 保活：装上 work guard，让 run() 在空闲（无连接）时也不返回，
  // 否则最后一个连接关闭后 io 线程会提前退出，导致无法重连。
  endpoint_.start_perpetual();
  endpoint_.set_open_handler(bind(&WebSocketSignalingClient::OnOpen, this, ::_1));
  endpoint_.set_close_handler(
      bind(&WebSocketSignalingClient::OnClose, this, ::_1));
  endpoint_.set_fail_handler(bind(&WebSocketSignalingClient::OnFail, this, ::_1));
  endpoint_.set_message_handler(
      bind(&WebSocketSignalingClient::OnMessage, this, ::_1, ::_2));
}

WebSocketSignalingClient::~WebSocketSignalingClient() {
  Close();
}

void WebSocketSignalingClient::SetCallbackExecutor(
    std::function<void(std::function<void()>)> executor) {
  executor_ = std::move(executor);
}

void WebSocketSignalingClient::RegisterObserver(
    SignalingClientObserver* callback) {
  observer_ = callback;
}

void WebSocketSignalingClient::Connect(const std::string& server, int port,
                                       const std::string& name) {
  // 若已有活跃连接，先软关闭旧的，避免新旧连接并存（on_close 靠 hdl_ 判断，不会误伤新连接）。
  {
    std::lock_guard<std::mutex> lk(mutex_);
    if (connected_) {
      websocketpp::lib::error_code ec;
      endpoint_.close(hdl_, websocketpp::close::status::normal, "reconnect", ec);
    }
  }

  websocketpp::lib::error_code ec;
  client::connection_ptr con;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    uri_ = "ws://" + server + ":" + std::to_string(port);
    client_name_ = name;
    con = endpoint_.get_connection(uri_, ec);
  }
  if (ec) {
    Fire([](SignalingClientObserver* o) {
      o->OnError("connect", "无法连接服务器");
    });
    return;
  }
  endpoint_.connect(con);
  if (!running_) {
    running_ = true;
    io_thread_ = std::thread([this] { endpoint_.run(); });
  }
}

void WebSocketSignalingClient::Close() {
  if (!running_) {
    return;
  }

  // 优雅关闭：发 close frame，让服务器检测到断开。
  {
    std::lock_guard<std::mutex> lk(mutex_);
    if (connected_) {
      websocketpp::lib::error_code ec;
      endpoint_.close(hdl_, websocketpp::close::status::normal, "close", ec);
    }
  }

  // 等 close 握手完成（on_close 置 close_done_），最多 1 秒。
  {
    std::unique_lock<std::mutex> lk(close_mutex_);
    close_cv_.wait_for(lk, std::chrono::milliseconds(1000),
                       [this] { return close_done_; });
  }

  running_ = false;
  endpoint_.stop();
  if (io_thread_.joinable()) {
    io_thread_.join();
  }
}

void WebSocketSignalingClient::Disconnect() {
  // 软断开：只发 close 帧关当前连接，保留 io 线程运行，便于再次 Connect()。
  websocketpp::lib::error_code ec;
  std::lock_guard<std::mutex> lk(mutex_);
  if (connected_) {
    endpoint_.close(hdl_, websocketpp::close::status::normal, "disconnect", ec);
  }
}

bool WebSocketSignalingClient::is_connected() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return connected_;
}

int WebSocketSignalingClient::id() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return my_id_;
}

bool WebSocketSignalingClient::Call(int peer_id, const std::string& sdp) {
  Json::Value v;
  v["type"] = "call";
  v["to"] = peer_id;
  v["sdp"] = sdp;
  return SendJson(SerializeJson(v));
}

bool WebSocketSignalingClient::Answer(int peer_id, const std::string& sdp) {
  Json::Value v;
  v["type"] = "answer";
  v["to"] = peer_id;
  v["sdp"] = sdp;
  return SendJson(SerializeJson(v));
}

bool WebSocketSignalingClient::Candidate(int peer_id,
                                         const std::string& candidate,
                                         const std::string& sdp_mid,
                                         int sdp_mline_index) {
  Json::Value v;
  v["type"] = "candidate";
  v["to"] = peer_id;
  v["candidate"] = candidate;
  v["sdpMid"] = sdp_mid;
  v["sdpMLineIndex"] = sdp_mline_index;
  return SendJson(SerializeJson(v));
}

bool WebSocketSignalingClient::Leave() {
  return SendJson(R"({"type":"leave"})");
}

bool WebSocketSignalingClient::GetRoster() {
  return SendJson(R"({"type":"get_roster"})");
}

bool WebSocketSignalingClient::SendMessage(int peer_id,
                                           const std::string& text) {
  Json::Value v;
  v["type"] = "message";
  v["to"] = peer_id;
  v["text"] = text;
  return SendJson(SerializeJson(v));
}

void WebSocketSignalingClient::OnOpen(websocketpp::connection_hdl hdl) {
  {
    std::lock_guard<std::mutex> lk(mutex_);
    connected_ = true;
    hdl_ = hdl;
  }
  Json::Value v;
  v["type"] = "register";
  v["name"] = client_name_;
  SendJson(SerializeJson(v));
}

void WebSocketSignalingClient::OnClose(websocketpp::connection_hdl hdl) {
  bool was_current = false;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!hdl.owner_before(hdl_) && !hdl_.owner_before(hdl)) {
      connected_ = false;
      was_current = true;
    }
  }
  {
    std::lock_guard<std::mutex> lk(close_mutex_);
    close_done_ = true;
  }
  close_cv_.notify_all();
  if (was_current) {
    Fire([](SignalingClientObserver* o) { o->OnDisconnected(); });
  }
}

void WebSocketSignalingClient::OnFail(websocketpp::connection_hdl hdl) {
  OnClose(hdl);
}

void WebSocketSignalingClient::OnMessage(websocketpp::connection_hdl,
                                         client::message_ptr msg) {
  Json::Value j;
  if (!ParseJson(msg->get_payload(), &j)) {
    return;
  }
  DispatchMessage(j);
}

void WebSocketSignalingClient::DispatchMessage(const Json::Value& j) {
  std::string type = GetStringField(j, "type");

  if (type == "welcome") {
    int id = GetIntField(j, "id", 0);
    {
      std::lock_guard<std::mutex> lk(mutex_);
      my_id_ = id;
    }
    Fire([id](SignalingClientObserver* o) { o->OnSignedIn(id); });
  } else if (type == "roster") {
    std::vector<PeerInfo> peers;
    if (j.isMember("peers") && j["peers"].isArray()) {
      for (const auto& p : j["peers"]) {
        PeerInfo pi;
        pi.id = GetIntField(p, "id", 0);
        pi.name = GetStringField(p, "name");
        pi.state = GetStringField(p, "state");
        peers.push_back(std::move(pi));
      }
    }
    Fire([peers](SignalingClientObserver* o) { o->OnRoster(peers); });
  } else if (type == "call") {
    int from = GetIntField(j, "from", 0);
    std::string sdp = GetStringField(j, "sdp");
    Fire([from, sdp](SignalingClientObserver* o) { o->OnCall(from, sdp); });
  } else if (type == "answer") {
    int from = GetIntField(j, "from", 0);
    std::string sdp = GetStringField(j, "sdp");
    Fire([from, sdp](SignalingClientObserver* o) { o->OnAnswer(from, sdp); });
  } else if (type == "candidate") {
    int from = GetIntField(j, "from", 0);
    std::string cand = GetStringField(j, "candidate");
    std::string mid = GetStringField(j, "sdpMid");
    int idx = GetIntField(j, "sdpMLineIndex", 0);
    Fire([from, cand, mid, idx](SignalingClientObserver* o) {
      o->OnCandidate(from, cand, mid, idx);
    });
  } else if (type == "leave") {
    int from = GetIntField(j, "from", 0);
    Fire([from](SignalingClientObserver* o) { o->OnLeave(from); });
  } else if (type == "peer_gone") {
    int from = GetIntField(j, "from", 0);
    Fire([from](SignalingClientObserver* o) { o->OnPeerGone(from); });
  } else if (type == "error") {
    std::string code = GetStringField(j, "code");
    std::string emsg = GetStringField(j, "msg");
    Fire([code, emsg](SignalingClientObserver* o) { o->OnError(code, emsg); });
  } else if (type == "message") {
    int from = GetIntField(j, "from", 0);
    std::string text = GetStringField(j, "text");
    Fire([from, text](SignalingClientObserver* o) { o->OnMessage(from, text); });
  }
  // 未知类型忽略
}

void WebSocketSignalingClient::Fire(
    const std::function<void(SignalingClientObserver*)>& fn) {
  SignalingClientObserver* obs = observer_;
  if (!obs) {
    return;
  }
  if (executor_) {
    executor_([obs, fn] { fn(obs); });
  } else {
    fn(obs);
  }
}

bool WebSocketSignalingClient::SendJson(const std::string& json) {
  websocketpp::connection_hdl h;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!connected_) {
      return false;
    }
    h = hdl_;
  }
  websocketpp::lib::error_code ec;
  endpoint_.send(h, json, websocketpp::frame::opcode::text, ec);
  return !ec;
}
