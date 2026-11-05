#include "webrtc_engine.h"

#include <iostream>

int main() {
  // 单次：三线程 + 工厂 + PC + DC
  {
    webrtc_engine engine;
    if (!engine.Start()) {
      std::cerr << "FAIL: Start() false\n";
      return 1;
    }
    if (!engine.InitializePeerConnectionFactory()) {
      std::cerr << "FAIL: InitializePeerConnectionFactory() false\n";
      return 1;
    }
    if (!engine.CreatePeerConnection()) {
      std::cerr << "FAIL: CreatePeerConnection() false\n";
      return 1;
    }
    if (!engine.CreateDataChannel()) {
      std::cerr << "FAIL: CreateDataChannel() false\n";
      return 1;
    }
    if (!engine.is_peer_connection_created() ||
        !engine.is_data_channel_created()) {
      std::cerr << "FAIL: PC/DC not marked created\n";
      return 1;
    }
    engine.Stop();
  }

  // 反复创建/销毁（含工厂/PC/DC，验证析构顺序安全、无泄漏崩溃）
  for (int i = 0; i < 20; i++) {
    webrtc_engine engine;
    if (!engine.Start()) {
      std::cerr << "FAIL: Start() false at iteration " << i << "\n";
      return 1;
    }
    if (!engine.InitializePeerConnectionFactory() ||
        !engine.CreatePeerConnection() || !engine.CreateDataChannel()) {
      std::cerr << "FAIL: create failed at iteration " << i << "\n";
      return 1;
    }
    engine.Stop();
  }

  std::cout << "webrtc_engine_smoke: all pass\n";
  return 0;
}
