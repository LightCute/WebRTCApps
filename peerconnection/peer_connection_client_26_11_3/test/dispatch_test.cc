#include <json/json.h>

#include <iostream>
#include <memory>
#include <string>

#include "recording_observer.h"
#include "ws_signaling_client.h"

#define CHECK(cond)                                \
  do {                                             \
    if (!(cond)) {                                 \
      std::cerr << "FAIL: " #cond " @ line "       \
                << __LINE__ << "\n";               \
      return 1;                                    \
    }                                              \
  } while (0)

namespace {

Json::Value Parse(const std::string& s) {
  Json::Value v;
  Json::CharReaderBuilder b;
  std::unique_ptr<Json::CharReader> r(b.newCharReader());
  std::string errs;
  r->parse(s.data(), s.data() + s.size(), &v, &errs);
  return v;
}

class TestableClient : public WebSocketSignalingClient {
 public:
  using WebSocketSignalingClient::DispatchMessage;  // 暴露 protected 方法
};

}  // namespace

int main() {
  TestableClient client;
  RecordingObserver obs;
  client.RegisterObserver(&obs);

  // welcome
  client.DispatchMessage(Parse(R"({"type":"welcome","id":7})"));
  {
    auto ev = obs.Snapshot();
    CHECK(ev.back().kind == RecordingObserver::kSignedIn && ev.back().id == 7);
  }
  CHECK(client.id() == 7);

  // roster
  client.DispatchMessage(Parse(
      R"({"type":"roster","peers":[{"id":1,"name":"a","state":"idle"},{"id":2,"name":"b","state":"busy"}]})"));
  {
    auto ev = obs.Snapshot();
    CHECK(ev.back().kind == RecordingObserver::kRoster && ev.back().s1 == "2");
  }

  // call
  client.DispatchMessage(Parse(R"({"type":"call","from":2,"sdp":"v=0"})"));
  {
    auto ev = obs.Snapshot();
    CHECK(ev.back().kind == RecordingObserver::kCall && ev.back().id == 2 &&
          ev.back().s1 == "v=0");
  }

  // answer
  client.DispatchMessage(Parse(R"({"type":"answer","from":2,"sdp":"a"})"));
  {
    auto ev = obs.Snapshot();
    CHECK(ev.back().kind == RecordingObserver::kAnswer && ev.back().id == 2 &&
          ev.back().s1 == "a");
  }

  // candidate
  client.DispatchMessage(Parse(
      R"({"type":"candidate","from":2,"candidate":"cand","sdpMid":"0","sdpMLineIndex":3})"));
  {
    auto ev = obs.Snapshot();
    CHECK(ev.back().kind == RecordingObserver::kCandidate &&
          ev.back().id == 2 && ev.back().s1 == "cand" && ev.back().s2 == "0" &&
          ev.back().n == 3);
  }

  // leave
  client.DispatchMessage(Parse(R"({"type":"leave","from":2})"));
  {
    auto ev = obs.Snapshot();
    CHECK(ev.back().kind == RecordingObserver::kLeave && ev.back().id == 2);
  }

  // peer_gone
  client.DispatchMessage(Parse(R"({"type":"peer_gone","from":3})"));
  {
    auto ev = obs.Snapshot();
    CHECK(ev.back().kind == RecordingObserver::kPeerGone && ev.back().id == 3);
  }

  // message
  client.DispatchMessage(Parse(R"({"type":"message","from":2,"text":"hi"})"));
  {
    auto ev = obs.Snapshot();
    CHECK(ev.back().kind == RecordingObserver::kMessage && ev.back().id == 2 &&
          ev.back().s1 == "hi");
  }

  // error
  client.DispatchMessage(Parse(R"({"type":"error","code":"busy","msg":"忙"})"));
  {
    auto ev = obs.Snapshot();
    CHECK(ev.back().kind == RecordingObserver::kError && ev.back().s1 == "busy" &&
          ev.back().s2 == "忙");
  }

  std::cout << "dispatch_test: all pass\n";
  return 0;
}
