#ifndef PEER_CONNECTION_CLIENT_TEST_RECORDING_OBSERVER_H_
#define PEER_CONNECTION_CLIENT_TEST_RECORDING_OBSERVER_H_

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "ws_signaling_client.h"

// 测试用 observer：线程安全地记录所有回调，供 WaitFor/Snapshot 断言。
class RecordingObserver : public SignalingClientObserver {
 public:
  enum Kind {
    kSignedIn,
    kDisconnected,
    kRoster,
    kCall,
    kAnswer,
    kCandidate,
    kLeave,
    kPeerGone,
    kError,
    kMessage,
  };

  struct Event {
    Kind kind;
    int id;
    std::string s1;  // sdp / candidate / code
    std::string s2;  // sdp_mid / msg
    int n;           // sdp_mline_index
    Event(Kind k, int i = -1, std::string a = "", std::string b = "",
          int m = 0)
        : kind(k), id(i), s1(std::move(a)), s2(std::move(b)), n(m) {}
  };

  void OnSignedIn(int my_id) override { Record(Event(kSignedIn, my_id)); }
  void OnDisconnected() override { Record(Event(kDisconnected)); }
  void OnRoster(const std::vector<PeerInfo>& peers) override {
    Record(Event(kRoster, -1, std::to_string(peers.size())));
  }
  void OnCall(int from, const std::string& sdp) override {
    Record(Event(kCall, from, sdp));
  }
  void OnAnswer(int from, const std::string& sdp) override {
    Record(Event(kAnswer, from, sdp));
  }
  void OnCandidate(int from, const std::string& candidate,
                   const std::string& sdp_mid, int idx) override {
    Record(Event(kCandidate, from, candidate, sdp_mid, idx));
  }
  void OnLeave(int from) override { Record(Event(kLeave, from)); }
  void OnPeerGone(int from) override { Record(Event(kPeerGone, from)); }
  void OnError(const std::string& code, const std::string& msg) override {
    Record(Event(kError, -1, code, msg));
  }
  void OnMessage(int from, const std::string& text) override {
    Record(Event(kMessage, from, text));
  }

  bool WaitFor(const std::function<bool(const std::vector<Event>&)>& pred,
               int timeout_ms) {
    std::unique_lock<std::mutex> lk(mutex_);
    return cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                        [&] { return pred(events_); });
  }

  std::vector<Event> Snapshot() {
    std::lock_guard<std::mutex> lk(mutex_);
    return events_;
  }

 protected:
  void Record(Event e) {
    {
      std::lock_guard<std::mutex> lk(mutex_);
      events_.push_back(std::move(e));
    }
    cv_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<Event> events_;
};

#endif  // PEER_CONNECTION_CLIENT_TEST_RECORDING_OBSERVER_H_
