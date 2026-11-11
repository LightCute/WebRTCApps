#ifndef APPS_PEERCONNECTION_WEBRTC_CLIENT_WEBRTC_ENGINE_H_
#define APPS_PEERCONNECTION_WEBRTC_CLIENT_WEBRTC_ENGINE_H_

#include <memory>
#include <string>
#include <vector>

#include "api/data_channel_interface.h"
#include "api/jsep.h"
#include "api/make_ref_counted.h"
#include "api/peer_connection_interface.h"
#include "api/rtc_error.h"
#include "api/scoped_refptr.h"
#include "api/set_local_description_observer_interface.h"
#include "api/stats/rtc_stats_collector_callback.h"
#include "api/stats/rtc_stats_report.h"
#include "api/set_remote_description_observer_interface.h"
#include "engine_observer.h"
#include "ice_config.h"
#include "rtc_base/thread.h"
#include "signaling/ws_signaling_client.h"

// WebRTC 客户端核心类：
// 三线程 + 信令客户端 + PeerConnectionFactory/PeerConnection/DataChannel +
// offer/answer 流程。自身实现三个长生命周期观察者接口，避免全局变量。
class webrtc_engine : public SignalingClientObserver,
                      public webrtc::PeerConnectionObserver,
                      public webrtc::DataChannelObserver {
 public:
  webrtc_engine();
  ~webrtc_engine();

  webrtc_engine(const webrtc_engine&) = delete;
  webrtc_engine& operator=(const webrtc_engine&) = delete;

  bool Start();
  void Stop();

  bool is_running() const { return signaling_thread_ != nullptr; }
  webrtc::Thread* signaling_thread() const { return signaling_thread_.get(); }
  webrtc::Thread* worker_thread() const { return worker_thread_.get(); }
  webrtc::Thread* network_thread() const { return network_thread_.get(); }

  void ConnectToServer(const std::string& server, int port,
                       const std::string& client_name);
  void DisconnectFromServer();
  // 软断开信令服务器（保留 io 线程，可再次 ConnectToServer 重连）。
  void DisconnectSignal();
  void RegisterObserver(EngineObserver* observer);
  // 配置 ICE（STUN/TURN + 传输模式），须在首次 call 前调用。
  void ConfigureIce(const IceConfig& cfg);
  // 运行时切换 ICE 模式（空闲时用，下次 call 生效；只改 mode，server 沿用）。
  void SetIceTransports(const std::string& mode);
  // 通话中切换本端 ICE 模式 + 重建 PC 重新连接（通知对端 switch_ice，
  // 由 offerer 重建后重新 offer；对端 mode 不变）。
  void SwitchIce(const std::string& mode);
  // 当前 ICE 配置（供前端回显）。
  const IceConfig& ice_config() const { return ice_config_; }

  bool is_signed_in() const { return signed_in_; }
  int my_id() const { return my_id_; }
  bool callback_on_signaling_thread() const {
    return callback_on_signaling_thread_;
  }

  bool InitializePeerConnectionFactory();
  bool CreatePeerConnection();
  bool CreateDataChannel();

  bool is_peer_connection_created() const { return peer_connection_ != nullptr; }
  bool is_data_channel_created() const { return data_channel_ != nullptr; }

  // 呼叫：主叫侧发起（建 PC/DC + CreateOffer）。
  void StartCall(int peer_id);
  // 挂断当前通话（发 leave，服务器通知双方清理）。
  void Leave();
  // 主动查询在线列表。
  void GetRoster();
  // 请求采集运行时状态（DataChannel 同步信息 + PeerConnection GetStats），
  // 结果经 EngineObserver::OnStats 回调上抛。
  void RequestStats();
  // 通过 DataChannel 发消息。
  bool SendDataChannelMessage(const std::string& message);
  // 通过信令服务器转发文本消息（无需通话，双方在线即可）。
  bool SendSignalMessage(int peer_id, const std::string& text);

  // 通话状态（供上层/测试）。
  bool is_connected() const { return is_connected_; }
  bool is_data_channel_open() const { return data_channel_open_; }
  const std::string& last_received_message() const {
    return last_received_message_;
  }
  int peer_id() const { return peer_id_; }

  // SignalingClientObserver
  void OnSignalSignedIn(int my_id) override;
  void OnSignalDisconnected() override;
  void OnSignalRoster(const std::vector<PeerInfo>& peers) override;
  void OnSignalCall(int from, const std::string& sdp) override;
  void OnSignalAnswer(int from, const std::string& sdp) override;
  void OnSignalCandidate(int from, const std::string& candidate,
                   const std::string& sdp_mid, int sdp_mline_index) override;
  void OnSignalLeave(int from) override;
  void OnSignalPeerGone(int from) override;
  void OnSignalMessage(int from, const std::string& text) override;
  void OnSignalError(const std::string& code, const std::string& msg) override;
  void OnSignalSwitchIce(int from) override;
  void OnSignalReoffer(int from, const std::string& sdp) override;

  // PeerConnectionObserver
  void OnSignalingChange(
      webrtc::PeerConnectionInterface::SignalingState new_state) override;
  void OnDataChannel(
      webrtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) override;
  void OnIceGatheringChange(
      webrtc::PeerConnectionInterface::IceGatheringState new_state) override;
  void OnIceCandidate(const webrtc::IceCandidate* candidate) override;
  void OnConnectionChange(
      webrtc::PeerConnectionInterface::PeerConnectionState state) override;

  // DataChannelObserver
  void OnStateChange() override;
  void OnMessage(const webrtc::DataBuffer& buffer) override;

 private:
  // CreateOffer/CreateAnswer 回调（长期持有，CreateOffer/CreateAnswer 收裸指针）
  class CreateSdpObserver : public webrtc::CreateSessionDescriptionObserver {
   public:
    explicit CreateSdpObserver(webrtc_engine* engine) : engine_(engine) {}
    void OnSuccess(webrtc::SessionDescriptionInterface* desc) override;
    void OnFailure(webrtc::RTCError error) override;

   private:
    webrtc_engine* engine_;
  };

  // SetLocalDescription 回调（每次新建）
  class SetLocalObserver
      : public webrtc::SetLocalDescriptionObserverInterface {
   public:
    explicit SetLocalObserver(webrtc_engine* engine) : engine_(engine) {}
    void OnSetLocalDescriptionComplete(webrtc::RTCError error) override;

   private:
    webrtc_engine* engine_;
  };

  // SetRemoteDescription 回调（每次新建）
  class SetRemoteObserver
      : public webrtc::SetRemoteDescriptionObserverInterface {
   public:
    explicit SetRemoteObserver(webrtc_engine* engine) : engine_(engine) {}
    void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override;

   private:
    webrtc_engine* engine_;
  };

  // GetStats 回调：携带已采集的 DataChannel 快照，收到 RTCStatsReport 后补齐
  // transport/candidate-pair/pc 信息并上抛。
  class StatsCollector : public webrtc::RTCStatsCollectorCallback {
   public:
    StatsCollector(webrtc_engine* engine, EngineStats stats);
    void OnStatsDelivered(
        const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report)
        override;

   private:
    webrtc_engine* engine_;
    EngineStats stats_;
  };

  // 以下方法都必须在 signaling 线程执行（由外层 BlockingCall / 信令回调保证）。
  bool InitializePeerConnectionFactoryImpl();
  bool CreatePeerConnectionImpl();
  bool CreateDataChannelImpl();
  void StartCallImpl(int peer_id);
  bool SendDataChannelMessageImpl(const std::string& message);
  void CreateOffer();
  void CreateAnswer();
  void ReleasePeerConnection();   // 只拆 PC/DC，保留 factory 与通话身份
  void ReconnectAsOfferer();      // 释放旧 PC 后按本端 mode 重建并发起新 offer
  void SetRemoteOffer(const std::string& sdp);
  void SetRemoteAnswer(const std::string& sdp);
  void AddRemoteCandidate(const std::string& candidate,
                          const std::string& sdp_mid, int sdp_mline_index);
  void ClosePeerConnection();
  void RequestStatsImpl();
  void CollectDataChannelStatsImpl(EngineStats::DataChannelInfo* out);

  std::unique_ptr<webrtc::Thread> signaling_thread_;
  std::unique_ptr<webrtc::Thread> worker_thread_;
  std::unique_ptr<webrtc::Thread> network_thread_;

  WebSocketSignalingClient signaling_client_;

  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface>
      peer_connection_factory_;
  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection_;
  webrtc::scoped_refptr<webrtc::DataChannelInterface> data_channel_;
  webrtc::PeerConnectionInterface::RTCConfiguration rtc_configuration_;
  IceConfig ice_config_;  // 当前 ICE 配置（mode 可变，server 固定）
  webrtc::scoped_refptr<CreateSdpObserver> sdp_observer_;

  bool signed_in_ = false;
  int my_id_ = -1;
  bool callback_on_signaling_thread_ = false;
  std::vector<PeerInfo> peers_;
  EngineObserver* engine_observer_ = nullptr;

  int peer_id_ = -1;            // 当前通话对端 id
  bool is_offerer_ = false;     // 本轮是主叫(true)还是被叫(false)
  bool is_connected_ = false;   // PC 已连通
  bool data_channel_open_ = false;
  bool ice_switch_in_progress_ = false;  // 正在切换 ICE 模式（重建中）
  bool renegotiating_ = false;            // 本轮 CreateOffer 是重建 reoffer 而非首次 call
  std::string last_received_message_;
};

#endif  // APPS_PEERCONNECTION_WEBRTC_CLIENT_WEBRTC_ENGINE_H_
