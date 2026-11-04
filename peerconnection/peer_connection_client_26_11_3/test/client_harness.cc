#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "recording_observer.h"
#include "ws_signaling_client.h"

class HarnessObserver : public RecordingObserver {
 public:
  explicit HarnessObserver(WebSocketSignalingClient* c) : client_(c) {}

  void OnSignedIn(int my_id) override {
    RecordingObserver::OnSignedIn(my_id);
    std::cout << "EVENT signed_in id=" << my_id << std::endl;
  }
  void OnRoster(const std::vector<PeerInfo>& peers) override {
    RecordingObserver::OnRoster(peers);
    std::cout << "EVENT roster";
    for (const auto& p : peers) {
      std::cout << " " << p.id << ":" << p.name;
    }
    std::cout << std::endl;
  }
  void OnCall(int from, const std::string& sdp) override {
    RecordingObserver::OnCall(from, sdp);
    std::cout << "EVENT call from=" << from << std::endl;
    client_->Answer(from, "harness-answer-sdp");
  }
  void OnCandidate(int from, const std::string& candidate,
                   const std::string& sdp_mid, int idx) override {
    RecordingObserver::OnCandidate(from, candidate, sdp_mid, idx);
    std::cout << "EVENT candidate from=" << from << std::endl;
  }
  void OnLeave(int from) override {
    RecordingObserver::OnLeave(from);
    std::cout << "EVENT leave from=" << from << std::endl;
  }
  void OnPeerGone(int from) override {
    RecordingObserver::OnPeerGone(from);
    std::cout << "EVENT peer_gone from=" << from << std::endl;
  }
  void OnError(const std::string& code, const std::string& msg) override {
    RecordingObserver::OnError(code, msg);
    std::cout << "EVENT error code=" << code << std::endl;
  }
  void OnMessage(int from, const std::string& text) override {
    RecordingObserver::OnMessage(from, text);
    std::cout << "EVENT message from=" << from << " text=" << text << std::endl;
  }

 private:
  WebSocketSignalingClient* client_;
};

namespace {
bool HasLeave(const std::vector<RecordingObserver::Event>& ev) {
  for (const auto& e : ev) {
    if (e.kind == RecordingObserver::kLeave) {
      return true;
    }
  }
  return false;
}
}  // namespace

int main(int argc, char* argv[]) {
  int port = (argc >= 2) ? std::atoi(argv[1]) : 8888;
  WebSocketSignalingClient client;
  HarnessObserver obs(&client);
  client.RegisterObserver(&obs);
  client.Connect("127.0.0.1", port, "harness");

  bool ok = obs.WaitFor(HasLeave, 60000);
  client.Close();
  return ok ? 0 : 1;
}
