#ifndef SIGNAL_SERVER_COMMON_H_
#define SIGNAL_SERVER_COMMON_H_

#include <string>

enum class ClientState { kIdle, kBusy };

inline const char* StateToString(ClientState s) {
  return s == ClientState::kIdle ? "idle" : "busy";
}

struct PeerInfo {
  int id;
  std::string name;
  ClientState state;
};

#endif  // SIGNAL_SERVER_COMMON_H_
