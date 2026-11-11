#ifndef APPS_PEERCONNECTION_WEBRTC_CLIENT_ENGINE_CONTROLLER_H_
#define APPS_PEERCONNECTION_WEBRTC_CLIENT_ENGINE_CONTROLLER_H_

#include <string>
#include <vector>

#include "engine_observer.h"
#include "ipc/unix_socket_server.h"

class webrtc_engine;

// 前端与引擎之间的命令/事件分发中枢：把 unix socket 的文本命令转成 webrtc_engine
// 调用，把引擎事件转成 JSON 推回上层。同时实现 UnixSocketObserver 与 EngineObserver。
class EngineController : public UnixSocketObserver, public EngineObserver {
 public:
  EngineController(webrtc_engine* engine, UnixSocketServer* server,
            const std::string& signal_server, int signal_port);

  // UnixSocketObserver（io 线程回调，须快速返回）
  void OnClientConnected(int client_id) override;
  void OnLineReceived(int client_id, const std::string& line) override;
  void OnClientDisconnected(int client_id) override;

  // EngineObserver（signaling 线程回调）
  void OnSignalSignedIn(int my_id) override;
  void OnSignalDisconnected() override;
  void OnSignalRoster(const std::vector<PeerInfo>& peers) override;
  void OnSignalCall(int from) override;
  void OnSignalMessage(int from, const std::string& text) override;
  void OnCallEnded(int from) override;
  void OnSignalError(const std::string& code, const std::string& msg) override;
  void OnEngineError(const std::string& code, const std::string& msg) override;
  void OnLocalSdp(const std::string& type, const std::string& sdp) override;
  void OnRemoteSdp(const std::string& type, const std::string& sdp) override;
  void OnLocalCandidate(const std::string& candidate,
                        const std::string& sdp_mid, int sdp_mline_index) override;
  void OnRemoteCandidate(const std::string& candidate,
                         const std::string& sdp_mid, int sdp_mline_index) override;
  void OnPeerConnectionStateChange(
      webrtc::PeerConnectionInterface::PeerConnectionState state) override;
  void OnDataChannelStateChange(
      webrtc::DataChannelInterface::DataState state) override;
  void OnDataChannelMessage(const std::string& message) override;
  void OnStats(const EngineStats& stats) override;
  void OnSignalSwitchIce(int from) override;
  void OnSignalReconnect(int from) override;
  void OnIceSwitched(bool success) override;
  void OnReconnected(bool success) override;

 private:
  void HandleCommand(const std::string& line);  // signaling 线程执行
  void Send(const std::string& msg);
  void SendError(const std::string& code, const std::string& msg);
  void SendIceConfig();  // 回显当前 ICE 配置

  webrtc_engine* engine_;
  UnixSocketServer* server_;
  std::string signal_server_;
  int signal_port_;
  int client_id_ = -1;
  std::vector<PeerInfo> peers_;
};

#endif  // APPS_PEERCONNECTION_WEBRTC_CLIENT_ENGINE_CONTROLLER_H_
