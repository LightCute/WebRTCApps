#include "registry.h"

int ClientRegistry::Register(const std::string& name) {
  if (names_.count(name)) {
    return -1;
  }
  int id = next_id_++;
  Member m;
  m.name = name;
  members_[id] = m;
  names_.insert(name);
  return id;
}

ClientRegistry::CallResult ClientRegistry::Call(int caller_id, int target_id) {
  if (caller_id == target_id) {
    return CallResult::kTargetBusy;  // self-call rejected
  }
  auto it = members_.find(caller_id);
  if (it == members_.end() || it->second.state == ClientState::kBusy) {
    return CallResult::kCallerBusy;
  }
  auto jt = members_.find(target_id);
  if (jt == members_.end()) {
    return CallResult::kTargetOffline;
  }
  if (jt->second.state == ClientState::kBusy) {
    return CallResult::kTargetBusy;
  }
  it->second.state = ClientState::kBusy;
  it->second.peer_id = target_id;
  jt->second.state = ClientState::kBusy;
  jt->second.peer_id = caller_id;
  return CallResult::kOk;
}

bool ClientRegistry::IsPaired(int a, int b) const {
  auto it = members_.find(a);
  if (it == members_.end()) {
    return false;
  }
  auto jt = members_.find(b);
  if (jt == members_.end()) {
    return false;
  }
  return it->second.peer_id == b && jt->second.peer_id == a;
}

int ClientRegistry::GetPeer(int id) const {
  auto it = members_.find(id);
  if (it == members_.end()) {
    return 0;
  }
  return it->second.peer_id;
}

void ClientRegistry::Hangup(int a, int b) {
  auto it = members_.find(a);
  if (it != members_.end()) {
    it->second.state = ClientState::kIdle;
    it->second.peer_id = 0;
  }
  auto jt = members_.find(b);
  if (jt != members_.end()) {
    jt->second.state = ClientState::kIdle;
    jt->second.peer_id = 0;
  }
}

int ClientRegistry::Remove(int id) {
  auto it = members_.find(id);
  if (it == members_.end()) {
    return 0;
  }
  int peer = it->second.peer_id;
  names_.erase(it->second.name);
  members_.erase(it);
  if (peer != 0) {
    auto pt = members_.find(peer);
    if (pt != members_.end()) {
      pt->second.state = ClientState::kIdle;
      pt->second.peer_id = 0;
    }
  }
  return peer;
}

std::vector<PeerInfo> ClientRegistry::Roster() const {
  std::vector<PeerInfo> out;
  for (const auto& kv : members_) {
    PeerInfo p;
    p.id = kv.first;
    p.name = kv.second.name;
    p.state = kv.second.state;
    out.push_back(p);
  }
  return out;
}

bool ClientRegistry::Has(int id) const {
  return members_.count(id) > 0;
}
