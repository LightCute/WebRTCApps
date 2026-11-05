#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
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

}  // namespace

int main(int argc, char* argv[]) {
  int port = (argc >= 2) ? std::atoi(argv[1]) : 8888;
  const int kRounds = 20;

  webrtc_engine offerer;
  webrtc_engine answerer;

  if (!offerer.Start() || !answerer.Start()) {
    std::cerr << "FAIL: Start()\n";
    return 1;
  }
  offerer.ConnectToServer("127.0.0.1", port, "multicall_offerer");
  if (!WaitFor([&] { return offerer.is_signed_in(); }, 5000)) {
    std::cerr << "FAIL: offerer not signed in\n";
    return 1;
  }
  answerer.ConnectToServer("127.0.0.1", port, "multicall_answerer");
  if (!WaitFor([&] { return answerer.is_signed_in(); }, 5000)) {
    std::cerr << "FAIL: answerer not signed in\n";
    return 1;
  }

  int answerer_id = answerer.my_id();

  for (int round = 0; round < kRounds; round++) {
    // 建立通话
    offerer.StartCall(answerer_id);
    if (!WaitFor([&] {
          return offerer.is_connected() && answerer.is_connected();
        }, 20000)) {
      std::cerr << "FAIL: round " << round << " not connected\n";
      return 1;
    }
    if (!WaitFor([&] {
          return offerer.is_data_channel_open() && answerer.is_data_channel_open();
        }, 20000)) {
      std::cerr << "FAIL: round " << round << " DataChannel not open\n";
      return 1;
    }

    // 发消息（每轮消息不同，验证 DC 数据通路）
    std::string message = "msg-" + std::to_string(round);
    if (!offerer.SendDataChannelMessage(message)) {
      std::cerr << "FAIL: round " << round << " SendDataChannelMessage\n";
      return 1;
    }
    if (!WaitFor([&] { return answerer.last_received_message() == message; },
                 5000)) {
      std::cerr << "FAIL: round " << round << " message not delivered\n";
      return 1;
    }

    // 挂断
    offerer.Leave();
    if (!WaitFor([&] {
          return !offerer.is_connected() && !offerer.is_peer_connection_created() &&
                 !answerer.is_connected() && !answerer.is_peer_connection_created();
        }, 5000)) {
      std::cerr << "FAIL: round " << round << " cleanup incomplete\n";
      return 1;
    }

    std::cout << "round " << round << " ok\n";
  }

  std::cout << "multicall integration ok: " << kRounds << " rounds\n";

  offerer.DisconnectFromServer();
  answerer.DisconnectFromServer();
  offerer.Stop();
  answerer.Stop();
  return 0;
}
