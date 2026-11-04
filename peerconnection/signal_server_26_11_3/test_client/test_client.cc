#include <websocketpp/config/asio_no_tls_client.hpp>
#include <websocketpp/client.hpp>
#include <websocketpp/common/thread.hpp>

#include <json/json.h>

#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>

typedef websocketpp::client<websocketpp::config::asio_client> client;
using websocketpp::lib::placeholders::_1;
using websocketpp::lib::placeholders::_2;
using websocketpp::lib::bind;

// Connection state, guarded by g_mutex (written by the event-loop thread,
// read by the stdin thread).
client* g_client = nullptr;
websocketpp::connection_hdl g_hdl;
std::mutex g_mutex;
bool g_open = false;
std::string g_uri;

void on_open(client* c, websocketpp::connection_hdl hdl) {
  {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_client = c;
    g_hdl = hdl;
    g_open = true;
  }
  std::cout << "connected" << std::endl;
}

void on_close(client*, websocketpp::connection_hdl hdl) {
  bool was_current = false;
  {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!hdl.owner_before(g_hdl) && !g_hdl.owner_before(hdl)) {  // same connection
      g_open = false;
      was_current = true;
    }
  }
  if (was_current) {
    std::cout << "disconnected" << std::endl;
  }
}

void on_message(client*, websocketpp::connection_hdl, client::message_ptr msg) {
  std::cout << "<< " << msg->get_payload() << std::endl;
}

std::string serialize(const Json::Value& v) {
  Json::StreamWriterBuilder b;
  b["indentation"] = "";
  std::unique_ptr<Json::StreamWriter> w(b.newStreamWriter());
  std::ostringstream os;
  w->write(v, &os);
  return os.str();
}

void connect_new(client* c) {
  websocketpp::lib::error_code ec;
  client::connection_ptr con = c->get_connection(g_uri, ec);
  if (ec) {
    std::cout << "connect error: " << ec.message() << std::endl;
    return;
  }
  c->connect(con);
}

void send_json(const Json::Value& v) {
  std::lock_guard<std::mutex> lk(g_mutex);
  if (!g_open) {
    std::cout << "(not connected)" << std::endl;
    return;
  }
  websocketpp::lib::error_code ec;
  g_client->send(g_hdl, serialize(v), websocketpp::frame::opcode::text, ec);
  if (ec) {
    std::cout << "send failed: " << ec.message() << std::endl;
  }
}

int main(int argc, char* argv[]) {
  g_uri = (argc >= 2) ? argv[1] : "ws://127.0.0.1:8888";
  client c;
  c.clear_access_channels(websocketpp::log::alevel::all);
  c.clear_error_channels(websocketpp::log::elevel::all);
  c.init_asio();
  c.set_open_handler(bind(&on_open, &c, ::_1));
  c.set_close_handler(bind(&on_close, &c, ::_1));
  c.set_message_handler(bind(&on_message, &c, ::_1, ::_2));

  connect_new(&c);

  std::cout << "commands: register <name> | call <id> <sdp> | "
               "answer <id> <sdp> | candidate <id> <cand> <mid> <idx> | "
               "msg <id> <text> | leave | get_roster | reconnect | quit"
            << std::endl;

  websocketpp::lib::thread reader([&c]() {
    std::string line;
    while (std::getline(std::cin, line)) {
      if (line.empty()) {
        continue;
      }
      std::stringstream ss(line);
      std::string cmd;
      ss >> cmd;
      if (cmd == "quit") {
        c.stop();
        break;
      }
      if (cmd == "reconnect") {
        {
          std::lock_guard<std::mutex> lk(g_mutex);
          if (g_open) {
            websocketpp::lib::error_code ec;
            g_client->close(g_hdl, websocketpp::close::status::normal,
                            "reconnect", ec);
          }
        }
        connect_new(&c);
        continue;
      }

      Json::Value v;
      if (cmd == "register") {
        std::string name;
        ss >> name;
        if (name.empty()) {
          std::cout << "usage: register <name>" << std::endl;
          continue;
        }
        v["type"] = "register";
        v["name"] = name;
      } else if (cmd == "call" || cmd == "answer") {
        int to;
        std::string sdp;
        ss >> to;
        std::getline(ss, sdp);
        if (!sdp.empty() && sdp[0] == ' ') {
          sdp = sdp.substr(1);
        }
        v["type"] = cmd;
        v["to"] = to;
        v["sdp"] = sdp;
      } else if (cmd == "candidate") {
        int to, idx;
        std::string cand, mid;
        ss >> to >> cand >> mid >> idx;
        v["type"] = "candidate";
        v["to"] = to;
        v["candidate"] = cand;
        v["sdpMid"] = mid;
        v["sdpMLineIndex"] = idx;
      } else if (cmd == "msg") {
        int to;
        std::string text;
        ss >> to;
        std::getline(ss, text);
        if (!text.empty() && text[0] == ' ') {
          text = text.substr(1);
        }
        v["type"] = "message";
        v["to"] = to;
        v["text"] = text;
      } else if (cmd == "leave") {
        v["type"] = "leave";
      } else if (cmd == "get_roster") {
        v["type"] = "get_roster";
      } else {
        std::cout << "unknown command: " << cmd << std::endl;
        continue;
      }
      send_json(v);
    }
  });

  c.run();
  reader.join();
  return 0;
}
