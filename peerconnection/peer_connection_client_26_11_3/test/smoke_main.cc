#include "ws_signaling_client.h"

#include <iostream>
#include <vector>

struct Observer : SignalingClientObserver {
  void OnSignedIn(int) override {}
  void OnDisconnected() override {}
  void OnRoster(const std::vector<PeerInfo>&) override {}
  void OnCall(int, const std::string&) override {}
  void OnAnswer(int, const std::string&) override {}
  void OnCandidate(int, const std::string&, const std::string&, int) override {}
  void OnLeave(int) override {}
  void OnPeerGone(int) override {}
  void OnError(const std::string&, const std::string&) override {}
  void OnMessage(int, const std::string&) override {}
};

int main() {
  WebSocketSignalingClient c;
  Observer o;
  c.RegisterObserver(&o);
  c.Close();
  std::cout << "smoke ok\n";
  return 0;
}
