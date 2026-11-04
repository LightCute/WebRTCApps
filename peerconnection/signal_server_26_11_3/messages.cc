#include "messages.h"

#include <memory>
#include <sstream>

namespace {

std::string WriteCompact(const Json::Value& root) {
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  std::unique_ptr<Json::StreamWriter> writer(builder.newStreamWriter());
  std::ostringstream os;
  writer->write(root, &os);
  return os.str();
}

}  // namespace

bool ParseJson(const std::string& text, Json::Value& out) {
  out = Json::Value();
  if (text.empty()) {
    return false;
  }
  Json::CharReaderBuilder builder;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  std::string errs;
  return reader->parse(text.data(), text.data() + text.size(), &out, &errs);
}

std::string GetStringField(const Json::Value& v, const std::string& key,
                           const std::string& def) {
  if (!v.isObject() || !v.isMember(key) || !v[key].isString()) {
    return def;
  }
  return v[key].asString();
}

int GetIntField(const Json::Value& v, const std::string& key, int def) {
  if (!v.isObject() || !v.isMember(key) || !v[key].isInt()) {
    return def;
  }
  return v[key].asInt();
}

std::string BuildWelcome(int id) {
  Json::Value v;
  v["type"] = "welcome";
  v["id"] = id;
  return WriteCompact(v);
}

std::string BuildError(const std::string& code, const std::string& msg) {
  Json::Value v;
  v["type"] = "error";
  v["code"] = code;
  v["msg"] = msg;
  return WriteCompact(v);
}

std::string BuildRoster(const std::vector<PeerInfo>& peers) {
  Json::Value v;
  v["type"] = "roster";
  Json::Value arr(Json::arrayValue);
  for (const auto& p : peers) {
    Json::Value e;
    e["id"] = p.id;
    e["name"] = p.name;
    e["state"] = StateToString(p.state);
    arr.append(e);
  }
  v["peers"] = arr;
  return WriteCompact(v);
}

std::string BuildLeave(int from) {
  Json::Value v;
  v["type"] = "leave";
  v["from"] = from;
  return WriteCompact(v);
}

std::string BuildPeerGone(int from) {
  Json::Value v;
  v["type"] = "peer_gone";
  v["from"] = from;
  return WriteCompact(v);
}

std::string BuildMessage(int from, const std::string& text) {
  Json::Value v;
  v["type"] = "message";
  v["from"] = from;
  v["text"] = text;
  return WriteCompact(v);
}

std::string StampFrom(const Json::Value& incoming, int from) {
  Json::Value copy = incoming;
  copy["from"] = from;
  return WriteCompact(copy);
}
