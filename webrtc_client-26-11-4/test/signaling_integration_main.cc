#include "webrtc_engine.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

int main(int argc, char* argv[]) {
  int port = (argc >= 2) ? std::atoi(argv[1]) : 8888;

  webrtc_engine engine;
  if (!engine.Start()) {
    std::cerr << "FAIL: Start() false\n";
    return 1;
  }
  engine.ConnectToServer("127.0.0.1", port, "engine-test");

  // 等 signed_in（最多 5 秒）
  for (int i = 0; i < 50 && !engine.is_signed_in(); i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  if (!engine.is_signed_in()) {
    std::cerr << "FAIL: not signed in within timeout\n";
    engine.DisconnectFromServer();
    engine.Stop();
    return 1;
  }
  if (engine.my_id() <= 0) {
    std::cerr << "FAIL: invalid my_id\n";
    return 1;
  }
  if (!engine.callback_on_signaling_thread()) {
    std::cerr << "FAIL: callback did not run on signaling thread\n";
    return 1;
  }

  std::cout << "signaling integration ok: my_id=" << engine.my_id() << "\n";

  engine.DisconnectFromServer();
  engine.Stop();
  return 0;
}
