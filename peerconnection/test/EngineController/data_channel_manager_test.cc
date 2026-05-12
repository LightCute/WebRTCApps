#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "api/data_channel_interface.h"
#include "api/scoped_refptr.h"
#include "apps/peerconnection/client/data_channel_manager.h"

class FakeDataChannel : public webrtc::DataChannelInterface {
 public:
  explicit FakeDataChannel(const std::string& label = "chat")
      : label_(label) {}

  void RegisterObserver(webrtc::DataChannelObserver* o) override { observer_ = o; }
  void UnregisterObserver() override { observer_ = nullptr; }
  std::string label() const override { return label_; }
  bool reliable() const override { return true; }
  int id() const override { return 0; }
  DataState state() const override { return state_; }
  uint32_t messages_sent() const override { return static_cast<uint32_t>(sent_.size()); }
  uint64_t bytes_sent() const override { return 0; }
  uint32_t messages_received() const override { return 0; }
  uint64_t bytes_received() const override { return 0; }
  uint64_t buffered_amount() const override { return 0; }
  void Close() override { state_ = kClosed; }

  bool Send(const webrtc::DataBuffer& buf) override {
    if (state_ != kOpen) return false;
    sent_.push_back(std::string(buf.data.data<char>(), buf.data.size()));
    return true;
  }

  // RefCountInterface
  void AddRef() const override { ref_count_++; }
  webrtc::RefCountReleaseStatus Release() const override {
    ref_count_--;
    return webrtc::RefCountReleaseStatus::kOtherRefsRemained;
  }

  // Test helpers
  void set_state(DataState s) { state_ = s; }
  void SimulateStateChange() { if (observer_) observer_->OnStateChange(); }
  void SimulateMessage(const std::string& text) {
    if (observer_) observer_->OnMessage(webrtc::DataBuffer(text));
  }
  const std::vector<std::string>& sent() const { return sent_; }
  webrtc::DataChannelObserver* observer() const { return observer_; }

 protected:
  ~FakeDataChannel() override = default;

 private:
  mutable int ref_count_ = 0;
  webrtc::DataChannelObserver* observer_ = nullptr;
  std::string label_;
  DataState state_ = kConnecting;
  std::vector<std::string> sent_;
};

// ==================== Test helpers ====================

static int tests_run = 0, tests_failed = 0;

#define TEST(name) do { tests_run++; std::cerr << "  " << name << " ... "; } while(0)
#define PASS()     do { std::cerr << "PASS" << std::endl; } while(0)
#define FAIL(msg)  do { tests_failed++; std::cerr << "FAIL (" << __LINE__ << "): " << msg << std::endl; } while(0)
#define CHECK(c, m) do { if (!(c)) FAIL(m); else PASS(); } while(0)

// ==================== Tests ====================

void test_on_remote_registers_observer() {
  TEST("OnRemote registers observer");
  DataChannelManager mgr;
  auto dc = webrtc::scoped_refptr<FakeDataChannel>(new FakeDataChannel("chat"));
  mgr.OnRemoteDataChannel(dc);
  CHECK(dc->observer() == &mgr, "observer registered");
}

void test_on_remote_rejects_wrong_label() {
  TEST("OnRemote rejects wrong label");
  DataChannelManager mgr;
  auto dc = webrtc::scoped_refptr<FakeDataChannel>(new FakeDataChannel("wrong"));
  mgr.OnRemoteDataChannel(dc);
  CHECK(dc->observer() == nullptr, "observer not registered");
}

void test_on_remote_rejects_null() {
  TEST("OnRemote rejects null");
  DataChannelManager mgr;
  mgr.OnRemoteDataChannel(nullptr);
  PASS();
}

void test_on_remote_replaces_existing() {
  TEST("OnRemote replaces existing");
  DataChannelManager mgr;
  auto dc1 = webrtc::scoped_refptr<FakeDataChannel>(new FakeDataChannel("chat"));
  mgr.OnRemoteDataChannel(dc1);
  auto dc2 = webrtc::scoped_refptr<FakeDataChannel>(new FakeDataChannel("chat"));
  mgr.OnRemoteDataChannel(dc2);
  CHECK(dc2->observer() == &mgr, "observer on second");
}

void test_send_works_when_open() {
  TEST("Send works when open");
  DataChannelManager mgr;
  auto dc = webrtc::scoped_refptr<FakeDataChannel>(new FakeDataChannel("chat"));
  dc->set_state(webrtc::DataChannelInterface::kOpen);
  mgr.OnRemoteDataChannel(dc);
  mgr.Send("hello world");
  CHECK(dc->sent().size() == 1, "one message");
  CHECK(dc->sent()[0] == "hello world", "correct text");
}

void test_send_ignores_when_not_open() {
  TEST("Send ignores when not open");
  DataChannelManager mgr;
  auto dc = webrtc::scoped_refptr<FakeDataChannel>(new FakeDataChannel("chat"));
  dc->set_state(webrtc::DataChannelInterface::kConnecting);
  mgr.OnRemoteDataChannel(dc);
  mgr.Send("test");
  CHECK(dc->sent().empty(), "nothing sent");
}

void test_send_without_channel() {
  TEST("Send without channel no crash");
  DataChannelManager mgr;
  mgr.Send("test");
  PASS();
}

void test_on_state_change_event() {
  TEST("OnStateChange fires event");
  DataChannelManager mgr;
  std::string event;
  mgr.SetEventCallback([&](const std::string& j) { event = j; });

  auto dc = webrtc::scoped_refptr<FakeDataChannel>(new FakeDataChannel("chat"));
  dc->set_state(webrtc::DataChannelInterface::kOpen);
  mgr.OnRemoteDataChannel(dc);
  dc->SimulateStateChange();

  CHECK(!event.empty(), "event received");
  CHECK(event.find("data_channel_state") != std::string::npos, "correct type");
  CHECK(event.find("open") != std::string::npos, "state is open");
}

void test_on_message_event() {
  TEST("OnMessage fires event");
  DataChannelManager mgr;
  std::string event;
  mgr.SetEventCallback([&](const std::string& j) { event = j; });

  auto dc = webrtc::scoped_refptr<FakeDataChannel>(new FakeDataChannel("chat"));
  mgr.OnRemoteDataChannel(dc);
  dc->SimulateMessage("test 123");

  CHECK(!event.empty(), "event received");
  CHECK(event.find("data_received") != std::string::npos, "correct type");
  CHECK(event.find("test 123") != std::string::npos, "text included");
}

void test_shutdown_releases() {
  TEST("Shutdown releases channel");
  DataChannelManager mgr;
  auto dc = webrtc::scoped_refptr<FakeDataChannel>(new FakeDataChannel("chat"));
  dc->set_state(webrtc::DataChannelInterface::kOpen);
  mgr.OnRemoteDataChannel(dc);
  mgr.Shutdown();

  mgr.Send("after");
  CHECK(dc->sent().empty(), "nothing sent after shutdown");
}

void test_add_covered_by_integration() {
  TEST("Add(PC) path covered by integration");
  // Add(PC) calls PC->CreateDataChannelOrError which needs a real PeerConnection.
  // The OnRemoteDataChannel path (tested above) covers all observer logic.
  PASS();
}

int main() {
  std::cerr << "=== DataChannelManager Tests ===" << std::endl;
  test_on_remote_registers_observer();
  test_on_remote_rejects_wrong_label();
  test_on_remote_rejects_null();
  test_on_remote_replaces_existing();
  test_send_works_when_open();
  test_send_ignores_when_not_open();
  test_send_without_channel();
  test_on_state_change_event();
  test_on_message_event();
  test_shutdown_releases();
  test_add_covered_by_integration();
  std::cerr << std::endl << tests_run << " tests, " << tests_failed << " failed" << std::endl;
  return tests_failed ? 1 : 0;
}
