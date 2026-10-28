#ifndef APPS_PEERCONNECTION_LEARNING_LEARNING_COMMON_H_
#define APPS_PEERCONNECTION_LEARNING_LEARNING_COMMON_H_

#include <string>

// 本端在 WebRTC DataChannel 会话中的角色。
enum class Role { kOfferer, kAnswerer };

// 跑一个完整的 WebRTC DataChannel 会话（阻塞，直到挂断/断开）。
// role:        本端角色
// socket_path: 监听的 Unix domain socket 路径
int RunSession(Role role, const std::string& socket_path);

#endif  // APPS_PEERCONNECTION_LEARNING_LEARNING_COMMON_H_
