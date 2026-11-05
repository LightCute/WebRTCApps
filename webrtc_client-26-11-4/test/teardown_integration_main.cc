#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <thread>

#include "webrtc_engine.h"

namespace {

bool WaitFor(const std::function<bool()>& cond, int timeout_ms) {
  int waited = 0;
  while (!cond()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    waited += 100;
    if (waited >= timeout_ms) {
      return false;
    }
  }
  return true;
}

// 建立一通通话（a 主叫、b 被叫），并等双方 PC 连通 + DC 打开。
bool EstablishCall(webrtc_engine& a, webrtc_engine& b, int port,
                   const std::string& name_a, const std::string& name_b) {
  if (!a.Start() || !b.Start()) {
    return false;
  }
  a.ConnectToServer("127.0.0.1", port, name_a);
  if (!WaitFor([&] { return a.is_signed_in(); }, 5000)) {
    return false;
  }
  b.ConnectToServer("127.0.0.1", port, name_b);
  if (!WaitFor([&] { return b.is_signed_in(); }, 5000)) {
    return false;
  }
  a.StartCall(b.my_id());
  if (!WaitFor([&] { return a.is_connected() && b.is_connected(); }, 20000)) {
    return false;
  }
  if (!WaitFor([&] {
        return a.is_data_channel_open() && b.is_data_channel_open();
      }, 20000)) {
    return false;
  }
  return true;
}

bool Teardown(webrtc_engine& a, webrtc_engine& b) {
  a.DisconnectFromServer();
  b.DisconnectFromServer();
  a.Stop();
  b.Stop();
  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  int port = (argc >= 2) ? std::atoi(argv[1]) : 8888;

  // 场景 1：通话中 A 主动 leave → 双方清理。
  {
    webrtc_engine a, b;
    if (!EstablishCall(a, b, port, "leave_a", "leave_b")) {
      std::cerr << "FAIL: establish call (leave scenario)\n";
      return 1;
    }
    a.Leave();
    if (!WaitFor([&] {
          return !a.is_connected() && !b.is_connected();
        }, 5000)) {
      std::cerr << "FAIL: leave did not disconnect both\n";
      return 1;
    }
    if (a.is_peer_connection_created() || b.is_peer_connection_created()) {
      std::cerr << "FAIL: leave did not clean up PC\n";
      return 1;
    }
    std::cout << "leave cleanup ok\n";
    Teardown(a, b);
  }

  // 场景 2：通话中 A 掉线（断开信令）→ B 收到 peer_gone 并清理。
  {
    webrtc_engine a, b;
    if (!EstablishCall(a, b, port, "drop_a", "drop_b")) {
      std::cerr << "FAIL: establish call (drop scenario)\n";
      return 1;
    }
    a.DisconnectFromServer();  // 模拟掉线（信令断开）
    if (!WaitFor([&] {
          return !b.is_connected() && !b.is_peer_connection_created();
        }, 5000)) {
      std::cerr << "FAIL: peer drop did not clean up B\n";
      return 1;
    }
    std::cout << "peer_gone cleanup ok\n";
    Teardown(a, b);
  }

  std::cout << "teardown integration ok\n";
  return 0;
}
