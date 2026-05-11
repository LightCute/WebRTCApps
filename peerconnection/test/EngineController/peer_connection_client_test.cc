#include <iostream>
#include <string>

#include "apps/peerconnection/client/peer_connection_client.h"

struct TestObserver : public PeerConnectionClientObserver {
  bool signed_in = false;
  void OnSignedIn() override { signed_in = true; }
  void OnDisconnected() override {}
  void OnPeerConnected(int, const std::string&) override {}
  void OnPeerDisconnected(int) override {}
  void OnPeerBusy(int) override {}
  void OnMessageFromPeer(int, const std::string&) override {}
  void OnMessageSent(int) override {}
  void OnServerConnectionFailure() override {}
};

static int tests_run = 0, tests_failed = 0;

#define TEST(n) do { tests_run++; std::cerr << "  " << n << " ... "; } while(0)
#define PASS()  do { std::cerr << "PASS" << std::endl; } while(0)
#define FAIL(m) do { tests_failed++; std::cerr << "FAIL: " << m << std::endl; } while(0)
#define CHECK(c, m) do { if (c) PASS(); else FAIL(m); } while(0)

void test_initial_state() {
  TEST("initial state");
  PeerConnectionClient client;
  CHECK(client.id() == -1, "id is -1");
  CHECK(!client.is_connected(), "not connected");
  CHECK(client.peers().empty(), "peers empty");
  CHECK(!client.IsSendingMessage(), "not sending");
}

void test_observer_registration() {
  TEST("observer registration");
  PeerConnectionClient client;
  TestObserver obs;
  client.RegisterObserver(&obs);
  CHECK(true, "no crash");
}

void test_sign_out_not_connected() {
  TEST("SignOut when not connected");
  PeerConnectionClient client;
  CHECK(client.SignOut(), "returns true");
}

void test_send_to_peer_not_connected() {
  TEST("SendToPeer fails when not connected");
  PeerConnectionClient client;
  CHECK(!client.SendToPeer(42, "hello"), "returns false");
}

void test_send_hangup_not_connected() {
  TEST("SendHangUp fails when not connected");
  PeerConnectionClient client;
  CHECK(!client.SendHangUp(42), "returns false");
}

int main() {
  std::cerr << "=== PeerConnectionClient Tests ===" << std::endl;
  test_initial_state();
  test_observer_registration();
  test_sign_out_not_connected();
  test_send_to_peer_not_connected();
  test_send_hangup_not_connected();

  std::cerr << std::endl << tests_run << " tests, " << tests_failed << " failed" << std::endl;
  return tests_failed ? 1 : 0;
}
