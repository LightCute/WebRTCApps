#ifndef PEER_CONNECTION_CLIENT_WS_SIGNALING_CLIENT_H_
#define PEER_CONNECTION_CLIENT_WS_SIGNALING_CLIENT_H_

#include <websocketpp/config/asio_no_tls_client.hpp>
#include <websocketpp/client.hpp>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Json {
class Value;
}

// 在线列表项（来自 roster）。
struct PeerInfo {
  int id = 0;
  std::string name;
  std::string state;  // "idle" | "busy"
};

// 信令事件回调：类型化，直接对应线上协议。
struct SignalingClientObserver {
  virtual ~SignalingClientObserver() = default;
  virtual void OnSignalSignedIn(int my_id) = 0;
  virtual void OnSignalDisconnected() = 0;
  virtual void OnSignalRoster(const std::vector<PeerInfo>& peers) = 0;
  virtual void OnSignalCall(int from, const std::string& sdp) = 0;
  virtual void OnSignalAnswer(int from, const std::string& sdp) = 0;
  virtual void OnSignalCandidate(int from, const std::string& candidate,
                           const std::string& sdp_mid, int sdp_mline_index) = 0;
  virtual void OnSignalLeave(int from) = 0;
  virtual void OnSignalPeerGone(int from) = 0;
  virtual void OnSignalError(const std::string& code, const std::string& msg) = 0;
  virtual void OnSignalMessage(int from, const std::string& text) = 0;
  virtual void OnSignalSwitchIce(int from) = 0;
  virtual void OnSignalReconnect(int from) = 0;
  virtual void OnSignalReoffer(int from, const std::string& sdp) = 0;
};

// WebSocket 信令客户端：直接收发线上协议（register/call/answer/candidate/
// leave/get_roster），不做任何翻译。内部一个 io 线程跑 websocketpp run()。
class WebSocketSignalingClient {
 public:
  WebSocketSignalingClient();
  ~WebSocketSignalingClient();

  // 注入回调执行器：把 observer 回调 marshal 到目标线程（默认 io 线程 inline）。
  void SetCallbackExecutor(std::function<void(std::function<void()>)> executor);
  void RegisterObserver(SignalingClientObserver* callback);

  void Connect(const std::string& server, int port, const std::string& name);
  void Close();
  // 软断开：只关当前连接，保留 io 线程运行，便于再次 Connect() 重连。
  void Disconnect();

  bool is_connected() const;
  int id() const;

  bool Call(int peer_id, const std::string& sdp);
  bool Answer(int peer_id, const std::string& sdp);
  bool Candidate(int peer_id, const std::string& candidate,
                 const std::string& sdp_mid, int sdp_mline_index);
  bool Leave();
  bool GetRoster();
  bool SendMessage(int peer_id, const std::string& text);
  bool SendSwitchIce(int peer_id);
  bool SendReconnect(int peer_id);
  bool SendReoffer(int peer_id, const std::string& sdp);

 protected:
  // 解析后的消息分发：virtual 便于测试子类暴露直接调用，以及将来扩展。
  virtual void DispatchMessage(const Json::Value& j);

 private:
  typedef websocketpp::client<websocketpp::config::asio_client> client;

  void OnOpen(websocketpp::connection_hdl hdl);
  void OnClose(websocketpp::connection_hdl hdl);
  void OnFail(websocketpp::connection_hdl hdl);
  void OnMessage(websocketpp::connection_hdl hdl, client::message_ptr msg);

  void Fire(const std::function<void(SignalingClientObserver*)>& fn);
  bool SendJson(const std::string& json);

  client endpoint_;
  std::thread io_thread_;
  std::atomic<bool> running_{false};

  mutable std::mutex mutex_;
  websocketpp::connection_hdl hdl_;
  bool connected_ = false;
  int my_id_ = 0;

  // 优雅关闭协调：Close() 等待 on_close 完成。
  std::mutex close_mutex_;
  std::condition_variable close_cv_;
  bool close_done_ = false;

  SignalingClientObserver* observer_ = nullptr;  // Connect 前设置，运行期只读
  std::function<void(std::function<void()>)> executor_;
  std::string uri_;
  std::string client_name_;
};

#endif  // PEER_CONNECTION_CLIENT_WS_SIGNALING_CLIENT_H_
