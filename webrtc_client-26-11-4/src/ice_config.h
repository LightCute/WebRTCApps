#ifndef APPS_WEBRTC_CLIENT_ICE_CONFIG_H_
#define APPS_WEBRTC_CLIENT_ICE_CONFIG_H_

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <string>

// ICE 传输模式：host=只本机候选, srflx=只 STUN 反射候选, relay=只 TURN 中继候选, auto=全部。
enum class IceTransportsMode { kHost, kSrflx, kRelay, kAuto };

// STUN/TURN + 传输模式配置（空 uri 表示不配该服务器）。
struct IceConfig {
  std::string stun_uri;    // 如 "stun:stun.l.google.com:19302"
  std::string turn_uri;    // 如 "turn:1.2.3.4:3478"
  std::string turn_user;
  std::string turn_pass;
  IceTransportsMode mode = IceTransportsMode::kAuto;
};

namespace ice_detail {

inline std::string Trim(const std::string& s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

inline std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

}  // namespace ice_detail

// 解析 "host"/"srflx"/"relay"/"auto"（大小写不敏感，未知值回 kAuto）。
inline IceTransportsMode ParseIceTransports(const std::string& s) {
  std::string v = ice_detail::Lower(ice_detail::Trim(s));
  if (v == "host") return IceTransportsMode::kHost;
  if (v == "srflx") return IceTransportsMode::kSrflx;
  if (v == "relay") return IceTransportsMode::kRelay;
  return IceTransportsMode::kAuto;
}

// 读取 .env 文件：每行 KEY=VALUE，# 开头为注释，空行忽略。文件不存在返回空 map。
inline std::map<std::string, std::string> LoadEnvFile(const std::string& path) {
  std::map<std::string, std::string> kv;
  std::ifstream in(path);
  if (!in.is_open()) return kv;
  std::string line;
  while (std::getline(in, line)) {
    std::string t = ice_detail::Trim(line);
    if (t.empty() || t[0] == '#') continue;
    size_t eq = t.find('=');
    if (eq == std::string::npos) continue;
    std::string key = ice_detail::Trim(t.substr(0, eq));
    std::string val = ice_detail::Trim(t.substr(eq + 1));
    if (!key.empty()) kv[key] = val;
  }
  return kv;
}

#endif  // APPS_WEBRTC_CLIENT_ICE_CONFIG_H_
