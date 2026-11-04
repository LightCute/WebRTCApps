#ifndef SIGNAL_SERVER_MESSAGES_H_
#define SIGNAL_SERVER_MESSAGES_H_

#include <string>
#include <vector>

#include <json/json.h>

#include "common.h"

// Parse a JSON string into `out`. Returns false on failure (and resets `out`).
bool ParseJson(const std::string& text, Json::Value& out);

// Field accessors with defaults for missing/wrong-type keys.
std::string GetStringField(const Json::Value& v, const std::string& key,
                           const std::string& def);
int GetIntField(const Json::Value& v, const std::string& key, int def);

// Build server-generated messages (compact JSON, no trailing newline).
std::string BuildWelcome(int id);
std::string BuildError(const std::string& code, const std::string& msg);
std::string BuildRoster(const std::vector<PeerInfo>& peers);
std::string BuildLeave(int from);
std::string BuildPeerGone(int from);
std::string BuildMessage(int from, const std::string& text);

// Copy `incoming`, set "from", and re-serialize (verbatim pass-through:
// all other fields are preserved).
std::string StampFrom(const Json::Value& incoming, int from);

#endif  // SIGNAL_SERVER_MESSAGES_H_
