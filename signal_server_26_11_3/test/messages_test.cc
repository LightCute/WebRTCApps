#include "messages.h"

#include <iostream>
#include <vector>

#define CHECK(cond)                                \
  do {                                             \
    if (!(cond)) {                                 \
      std::cerr << "FAIL: " #cond " @ line "       \
                << __LINE__ << "\n";               \
      return 1;                                    \
    }                                              \
  } while (0)

int main() {
  Json::Value v;

  CHECK(ParseJson(BuildWelcome(7), v));
  CHECK(GetIntField(v, "id", -1) == 7);
  CHECK(GetStringField(v, "type", "") == "welcome");

  CHECK(ParseJson(BuildError("busy", "对方忙线"), v));
  CHECK(GetStringField(v, "code", "") == "busy");

  CHECK(ParseJson(BuildLeave(3), v));
  CHECK(GetStringField(v, "type", "") == "leave");
  CHECK(GetIntField(v, "from", -1) == 3);

  CHECK(ParseJson(BuildMessage(3, "hello"), v));
  CHECK(GetStringField(v, "type", "") == "message");
  CHECK(GetIntField(v, "from", -1) == 3);
  CHECK(GetStringField(v, "text", "") == "hello");

  std::vector<PeerInfo> peers = {{1, "robot", ClientState::kIdle},
                                 {2, "mon", ClientState::kBusy}};
  CHECK(ParseJson(BuildRoster(peers), v));
  CHECK(v["peers"].size() == 2);
  CHECK(v["peers"][1]["state"].asString() == "busy");

  CHECK(ParseJson(
      R"({"type":"answer","to":1,"sdp":"x","usernameFragment":"uf"})", v));
  std::string out = StampFrom(v, 2);
  CHECK(ParseJson(out, v));
  CHECK(GetIntField(v, "from", -1) == 2);
  CHECK(GetIntField(v, "to", -1) == 1);
  CHECK(GetStringField(v, "usernameFragment", "") == "uf");

  std::string sdp = "a=ice-ufrag:\"abc\"\r\nb=x\n";
  Json::Value c;
  c["type"] = "call";
  c["to"] = 1;
  c["sdp"] = sdp;
  CHECK(ParseJson(StampFrom(c, 1), v));
  CHECK(GetStringField(v, "sdp", "") == sdp);

  CHECK(!ParseJson("{not json", v));
  CHECK(!ParseJson("", v));

  std::cout << "messages_test: all pass\n";
  return 0;
}
