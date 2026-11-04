#include "registry.h"

#include <iostream>

#define CHECK(cond)                                \
  do {                                             \
    if (!(cond)) {                                 \
      std::cerr << "FAIL: " #cond " @ line "       \
                << __LINE__ << "\n";               \
      return 1;                                    \
    }                                              \
  } while (0)

int main() {
  ClientRegistry r;

  CHECK(r.Register("robot") == 1);
  CHECK(r.Register("mon") == 2);
  CHECK(r.Register("robot") == -1);  // 名字撞车
  CHECK(r.Has(1) && r.Has(2) && !r.Has(999));

  CHECK(r.Call(1, 2) == ClientRegistry::CallResult::kOk);
  CHECK(r.IsPaired(1, 2) && r.IsPaired(2, 1));
  CHECK(r.GetPeer(1) == 2);
  CHECK(r.GetPeer(2) == 1);
  CHECK(r.GetPeer(999) == 0);  // 未注册
  CHECK(!r.IsPaired(1, 3));
  CHECK(r.Roster().size() == 2);

  auto is_busy = [&](int id) {
    for (const auto& p : r.Roster()) {
      if (p.id == id) {
        return p.state == ClientState::kBusy;
      }
    }
    return false;
  };
  CHECK(is_busy(1) && is_busy(2));  // call 后双方 busy

  CHECK(r.Register("camera") == 3);
  CHECK(r.Call(3, 1) == ClientRegistry::CallResult::kTargetBusy);
  CHECK(r.Call(1, 3) == ClientRegistry::CallResult::kCallerBusy);
  CHECK(r.Call(3, 999) == ClientRegistry::CallResult::kTargetOffline);
  CHECK(r.Call(1, 1) == ClientRegistry::CallResult::kTargetBusy);  // 自呼拒绝

  r.Hangup(1, 2);
  CHECK(!r.IsPaired(1, 2));  // 挂断后回 idle
  CHECK(r.GetPeer(1) == 0);
  CHECK(r.GetPeer(2) == 0);

  CHECK(r.Call(1, 2) == ClientRegistry::CallResult::kOk);
  CHECK(r.Remove(2) == 1);   // 通话中移除返回对端 id
  CHECK(!r.Has(2));
  CHECK(!r.IsPaired(1, 2));  // 对端 1 已被清为 idle（spec §6）
  CHECK(r.Remove(3) == 0);   // 非通话中移除返回 0

  std::cout << "registry_test: all pass\n";
  return 0;
}
