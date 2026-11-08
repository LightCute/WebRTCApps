#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "ice_config.h"
#include "webrtc_engine.h"

namespace {

bool WaitFor(const std::function<bool()>& cond, int timeout_ms) {
  int waited = 0;
  while (!cond()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    waited += 100;
    if (waited >= timeout_ms) return false;
  }
  return true;
}

// 关心 OnStats（候选类型）和 OnSignalRoster（找对端 id），其余事件空实现。
struct StatsObserver : public EngineObserver {
  EngineStats last_stats;
  bool got_stats = false;
  bool got_call = false;
  std::vector<PeerInfo> peers;
  void OnStats(const EngineStats& s) override {
    last_stats = s;
    got_stats = true;
  }
  void OnSignalRoster(const std::vector<PeerInfo>& p) override { peers = p; }
  void OnSignalSignedIn(int) override {}
  void OnSignalDisconnected() override {}
  void OnSignalCall(int) override { got_call = true; }
  void OnSignalMessage(int, const std::string&) override {}
  void OnCallEnded(int) override {}
  void OnSignalError(const std::string&, const std::string&) override {}
  void OnEngineError(const std::string&, const std::string&) override {}
  void OnLocalSdp(const std::string&, const std::string&) override {}
  void OnRemoteSdp(const std::string&, const std::string&) override {}
  void OnLocalCandidate(const std::string&, const std::string&, int) override {}
  void OnRemoteCandidate(const std::string&, const std::string&, int) override {}
  void OnPeerConnectionStateChange(
      webrtc::PeerConnectionInterface::PeerConnectionState) override {}
  void OnDataChannelStateChange(
      webrtc::DataChannelInterface::DataState) override {}
  void OnDataChannelMessage(const std::string&) override {}
};

}  // namespace

int main(int argc, char* argv[]) {
  std::string role = "offerer";
  std::string server = "127.0.0.1";
  std::string transports = "srflx";
  int port = 8888;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a.rfind("--role=", 0) == 0) {
      role = a.substr(7);
    } else if (a.rfind("--server=", 0) == 0) {
      server = a.substr(9);
    } else if (a.rfind("--ice_transports=", 0) == 0) {
      transports = a.substr(17);
    } else if (a.rfind("--port=", 0) == 0) {
      port = std::atoi(a.c_str() + 7);
    } else if (a == "--port" && i + 1 < argc) {
      port = std::atoi(argv[++i]);
    }
  }

  webrtc_engine engine;
  StatsObserver obs;
  engine.RegisterObserver(&obs);

  // 配谷歌 STUN，模式由 --ice_transports 决定（默认 srflx）。
  IceConfig cfg;
  cfg.stun_uri = "stun:stun.l.google.com:19302";
  cfg.mode = ParseIceTransports(transports);
  engine.ConfigureIce(cfg);

  if (!engine.Start()) {
    std::cerr << "FAIL: Start()\n";
    return 1;
  }
  engine.ConnectToServer(server, port, role);
  if (!WaitFor([&] { return engine.is_signed_in(); }, 10000)) {
    std::cerr << "FAIL: not signed in\n";
    return 1;
  }
  std::cout << "[" << role << "] signed in, my_id=" << engine.my_id() << "\n";

  if (role == "offerer") {
    // 等 answerer 出现在 roster。
    if (!WaitFor([&] {
          for (const auto& p : obs.peers)
            if (p.name == "answerer") return true;
          return false;
        }, 30000)) {
      std::cerr << "FAIL: answerer not in roster\n";
      return 1;
    }
    int answerer_id = 0;
    for (const auto& p : obs.peers)
      if (p.name == "answerer") answerer_id = p.id;
    std::cout << "[offerer] calling answerer id=" << answerer_id << "\n";
    engine.StartCall(answerer_id);
  } else {
    // answerer 被动等来电，引擎在 OnSignalCall 里自动应答。
    if (!WaitFor([&] { return obs.got_call; }, 30000)) {
      std::cerr << "FAIL: no incoming call\n";
      return 1;
    }
    std::cout << "[answerer] incoming call received\n";
  }

  // 等 ICE 连通（最多 10 秒）。srflx 候选收集在连通检查期间也会完成；
  // hairpin 环境连不上也没关系，下面按 got_srflx 判定候选是否收集到。
  WaitFor([&] { return engine.is_connected(); }, 10000);
  bool connected = engine.is_connected();

  engine.RequestStats();
  if (!WaitFor([&] { return obs.got_stats; }, 5000)) {
    std::cerr << "FAIL: no stats\n";
    return 1;
  }

  std::string selected;
  bool got_srflx = false;
  for (const auto& c : obs.last_stats.local_candidates) {
    if (c.selected) selected = c.candidate_type;
    if (c.candidate_type == "srflx") got_srflx = true;
  }

  std::cout << "[" << role << "] connected=" << (connected ? "yes" : "no")
            << " selected=" << (selected.empty() ? "none" : selected) << "\n";
  std::cout << "[" << role << "] local_candidates:";
  for (const auto& c : obs.last_stats.local_candidates)
    std::cout << " " << c.candidate_type << (c.selected ? "*" : "");
  std::cout << "\n";

  // answerer 报完结果后多等几秒再断开，让 offerer 也完成候选收集（否则 offerer 会
  // 因 peer_gone 被提前关掉 PC，拿不到候选）。
  if (role == "answerer") {
    std::this_thread::sleep_for(std::chrono::seconds(5));
  }

  engine.DisconnectFromServer();
  engine.Stop();

  if (!got_srflx) {
    std::cerr << "[" << role << "] FAIL: no srflx candidate gathered\n";
    return 1;
  }
  std::cout << "[" << role << "] ok: srflx candidate gathered\n";
  return 0;
}
