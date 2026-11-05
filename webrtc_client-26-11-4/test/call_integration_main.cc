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

}  // namespace

int main(int argc, char* argv[]) {
  int port = (argc >= 2) ? std::atoi(argv[1]) : 8888;

  webrtc_engine offerer;
  webrtc_engine answerer;

  if (!offerer.Start() || !answerer.Start()) {
    std::cerr << "FAIL: Start()\n";
    return 1;
  }

  offerer.ConnectToServer("127.0.0.1", port, "offerer");
  if (!WaitFor([&] { return offerer.is_signed_in(); }, 5000)) {
    std::cerr << "FAIL: offerer not signed in\n";
    return 1;
  }
  answerer.ConnectToServer("127.0.0.1", port, "answerer");
  if (!WaitFor([&] { return answerer.is_signed_in(); }, 5000)) {
    std::cerr << "FAIL: answerer not signed in\n";
    return 1;
  }

  int answerer_id = answerer.my_id();
  offerer.StartCall(answerer_id);

  if (!WaitFor([&] {
        return offerer.is_connected() && answerer.is_connected();
      }, 20000)) {
    std::cerr << "FAIL: PC not connected\n";
    return 1;
  }
  if (!WaitFor([&] {
        return offerer.is_data_channel_open() && answerer.is_data_channel_open();
      }, 20000)) {
    std::cerr << "FAIL: DataChannel not open\n";
    return 1;
  }

  if (!offerer.SendDataChannelMessage("hello")) {
    std::cerr << "FAIL: SendDataChannelMessage\n";
    return 1;
  }
  if (!WaitFor([&] { return answerer.last_received_message() == "hello"; },
               5000)) {
    std::cerr << "FAIL: answerer did not receive message\n";
    return 1;
  }

  std::cout << "call integration ok: connected, DataChannel open, message "
               "delivered\n";

  offerer.DisconnectFromServer();
  answerer.DisconnectFromServer();
  offerer.Stop();
  answerer.Stop();
  return 0;
}
