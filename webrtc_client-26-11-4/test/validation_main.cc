#include "ice_config.h"
#include "webrtc_engine.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int Fail(const std::string& msg) {
  std::cerr << "FAIL: " << msg << "\n";
  return 1;
}

bool Has(const std::vector<std::string>& v, const std::string& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

// 记录型观察者：实现全部 EngineObserver 接口为空，只记录 OnEngineError。
struct RecordingObserver : public EngineObserver {
  std::vector<std::string> engine_errors;
  void OnEngineError(const std::string& code, const std::string&) override {
    engine_errors.push_back(code);
  }
  void OnSignalSignedIn(int) override {}
  void OnSignalDisconnected() override {}
  void OnSignalRoster(const std::vector<PeerInfo>&) override {}
  void OnSignalCall(int) override {}
  void OnSignalMessage(int, const std::string&) override {}
  void OnCallEnded(int) override {}
  void OnSignalError(const std::string&, const std::string&) override {}
  void OnLocalSdp(const std::string&, const std::string&) override {}
  void OnRemoteSdp(const std::string&, const std::string&) override {}
  void OnLocalCandidate(const std::string&, const std::string&, int) override {}
  void OnRemoteCandidate(const std::string&, const std::string&, int) override {}
  void OnPeerConnectionStateChange(
      webrtc::PeerConnectionInterface::PeerConnectionState) override {}
  void OnDataChannelStateChange(
      webrtc::DataChannelInterface::DataState) override {}
  void OnDataChannelMessage(const std::string&) override {}
  void OnStats(const EngineStats&) override {}
};

}  // namespace

int main() {
  // === ICE 配置解析 ===
  {
    if (ParseIceTransports("host") != IceTransportsMode::kHost)
      return Fail("parse host");
    if (ParseIceTransports("SRFLX") != IceTransportsMode::kSrflx)
      return Fail("parse srflx (case-insensitive)");
    if (ParseIceTransports("relay") != IceTransportsMode::kRelay)
      return Fail("parse relay");
    if (ParseIceTransports("auto") != IceTransportsMode::kAuto)
      return Fail("parse auto");
    if (ParseIceTransports("bogus") != IceTransportsMode::kAuto)
      return Fail("parse bogus -> auto");

    const char* path = "/tmp/wc_env_test.env";
    {
      std::ofstream out(path);
      out << "# comment\n";
      out << "WEBRTC_STUN=stun:stun.l.google.com:19302\n";
      out << "ICE_TRANSPORTS=srflx\n";
      out << "EMPTY=\n";
      out << "\n";
    }
    auto kv = LoadEnvFile(path);
    if (kv["WEBRTC_STUN"] != "stun:stun.l.google.com:19302")
      return Fail("env stun");
    if (kv["ICE_TRANSPORTS"] != "srflx") return Fail("env transports");
    if (kv.count("EMPTY") == 0 || !kv["EMPTY"].empty())
      return Fail("env empty value");
    if (kv.count("# comment") != 0) return Fail("env comment parsed");
  }

  // === 引擎层 bool 语义 ===
  {
    webrtc_engine engine;
    if (!engine.Start()) return Fail("Start() false");
    if (engine.SendDataChannelMessage("x"))
      return Fail("SendDataChannelMessage should be false");
    if (engine.SendSignalMessage(1, "x"))
      return Fail("SendSignalMessage should be false");
    engine.Stop();
  }

  // === engine.error 内部失败事件 ===
  {
    RecordingObserver o;
    webrtc_engine e;
    e.RegisterObserver(&o);
    e.Start();
    if (e.CreatePeerConnection()) return Fail("PC should fail without factory");
    if (!Has(o.engine_errors, "peer_connection_failed"))
      return Fail("missing peer_connection_failed");
  }
  {
    RecordingObserver o;
    webrtc_engine e;
    e.RegisterObserver(&o);
    e.Start();
    if (e.CreateDataChannel()) return Fail("DC should fail without PC");
    if (!Has(o.engine_errors, "data_channel_failed"))
      return Fail("missing data_channel_failed");
  }
  {
    RecordingObserver o;
    webrtc_engine e;
    e.RegisterObserver(&o);
    e.Start();
    e.signaling_thread()->BlockingCall([&] { e.OnSignalCall(1, "not a valid sdp"); });
    if (!Has(o.engine_errors, "set_remote_desc_failed"))
      return Fail("missing set_remote_desc_failed");
  }

  std::cout << "validation ok: bool semantics + engine.error\n";
  return 0;
}
