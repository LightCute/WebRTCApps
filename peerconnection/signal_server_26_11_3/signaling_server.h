#ifndef SIGNAL_SERVER_SIGNALING_SERVER_H_
#define SIGNAL_SERVER_SIGNALING_SERVER_H_

#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include <websocketpp/common/asio.hpp>

#include <ctime>
#include <map>
#include <memory>
#include <string>

#include "logger.h"
#include "registry.h"

// websocketpp server endpoint + the registry state machine + heartbeat.
// Single-threaded: all handlers run inside endpoint_.run().
class SignalingServer {
 public:
  SignalingServer(int port, int ping_interval_s, int ping_timeout_s,
                  Logger& logger);
  void Run();
  void Stop();

 private:
  typedef websocketpp::server<websocketpp::config::asio> server;

  void OnOpen(websocketpp::connection_hdl hdl);
  void OnClose(websocketpp::connection_hdl hdl);
  void OnFail(websocketpp::connection_hdl hdl);
  void OnMessage(websocketpp::connection_hdl hdl, server::message_ptr msg);
  void OnPong(websocketpp::connection_hdl hdl, std::string payload);
  void OnHeartbeat(const websocketpp::lib::error_code& ec);

  void SendToHdl(websocketpp::connection_hdl hdl, const std::string& json);
  void SendTo(int id, const std::string& json);
  void BroadcastRoster();
  void DropConnection(websocketpp::connection_hdl hdl);
  void ScheduleHeartbeat();

  server endpoint_;
  ClientRegistry registry_;
  Logger& logger_;
  std::map<int, websocketpp::connection_hdl> id_to_hdl_;
  std::map<websocketpp::connection_hdl, int,
           std::owner_less<websocketpp::connection_hdl>> hdl_to_id_;
  std::map<websocketpp::connection_hdl, std::time_t,
           std::owner_less<websocketpp::connection_hdl>> last_pong_;

  int port_;
  int ping_interval_s_;
  int ping_timeout_s_;
  std::unique_ptr<websocketpp::lib::asio::steady_timer> timer_;
};

#endif  // SIGNAL_SERVER_SIGNALING_SERVER_H_
