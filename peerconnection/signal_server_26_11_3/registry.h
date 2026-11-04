#ifndef SIGNAL_SERVER_REGISTRY_H_
#define SIGNAL_SERVER_REGISTRY_H_

#include <map>
#include <set>
#include <string>
#include <vector>

#include "common.h"

// Pure state machine for online clients: id assignment, name uniqueness,
// call pairing, busy tracking. No websocketpp/jsoncpp dependency.
class ClientRegistry {
 public:
  enum class CallResult { kOk, kCallerBusy, kTargetOffline, kTargetBusy };

  // Registers a new client; returns its new id, or -1 if the name is taken.
  int Register(const std::string& name);

  // Attempts to pair caller with target. On kOk, both become busy and paired.
  CallResult Call(int caller_id, int target_id);

  // True iff a and b are currently in a call with each other.
  bool IsPaired(int a, int b) const;

  // The id of the peer a is in a call with, or 0 if a is not in a call.
  int GetPeer(int id) const;

  // Clears the call between a and b (both -> idle).
  void Hangup(int a, int b);

  // Removes a client, clears its peer's call state, and returns the peer id
  // (0 if the removed client was not in a call).
  int Remove(int id);

  std::vector<PeerInfo> Roster() const;

  bool Has(int id) const;

 private:
  struct Member {
    std::string name;
    ClientState state = ClientState::kIdle;
    int peer_id = 0;  // 0 = not in a call
  };

  std::map<int, Member> members_;
  std::set<std::string> names_;
  int next_id_ = 1;
};

#endif  // SIGNAL_SERVER_REGISTRY_H_
