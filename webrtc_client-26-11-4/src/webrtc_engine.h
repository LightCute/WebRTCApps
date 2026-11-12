#ifndef APPS_PEERCONNECTION_WEBRTC_CLIENT_WEBRTC_ENGINE_H_
#define APPS_PEERCONNECTION_WEBRTC_CLIENT_WEBRTC_ENGINE_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "api/data_channel_interface.h"
#include "api/jsep.h"
#include "api/make_ref_counted.h"
#include "api/media_stream_interface.h"
#include "api/peer_connection_interface.h"
#include "api/rtc_error.h"
#include "api/rtp_receiver_interface.h"
#include "api/rtp_sender_interface.h"
#include "api/scoped_refptr.h"
#include "api/set_local_description_observer_interface.h"
#include "api/stats/rtc_stats_collector_callback.h"
#include "api/stats/rtc_stats_report.h"
#include "api/set_remote_description_observer_interface.h"
#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"
#include "api/video/video_source_interface.h"
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
  // 通话中手动触发 ICE 重启（复用旧候选，不重建 PC、不改 mode）。
  void Reconnect();
  // 视频开关（第一阶段：合成测试图源，空闲时用，通话中暂不支持）。
  void StartVideo();
  void StopVideo();
  // 通话中开/关本端视频轨（AddTrack/RemoveTrack + 重新 offer/answer）。
  void VideoOn();
  void VideoOff();
  // 视频参数控制（通话中，SetParameters 动态改，不重协商）。
  void SetBitrate(int max_kbps);
  void SetFramerate(int fps);
  void SetDegradation(const std::string& mode);
  // 回读当前视频参数（供前端回显）。
  void GetVideoStatus();
  bool video_enabled() const { return video_enabled_; }
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
  void OnSignalReconnect(int from) override;
  void OnSignalReoffer(int from, const std::string& sdp) override;

  // PeerConnectionObserver
  void OnSignalingChange(
      webrtc::PeerConnectionInterface::SignalingState new_state) override;
  void OnDataChannel(
      webrtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) override;
  void OnAddTrack(
      webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
      const std::vector<webrtc::scoped_refptr<webrtc::MediaStreamInterface>>&
          streams) override;
  void OnRemoveTrack(
      webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) override;
  void OnIceGatheringChange(
      webrtc::PeerConnectionInterface::IceGatheringState new_state) override;
  void OnIceConnectionChange(
      webrtc::PeerConnectionInterface::IceConnectionState new_state) override;
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

  // SetRemoteDescription 回调（每次新建；is_offer 区分收的是 offer 还是 answer）
  class SetRemoteObserver
      : public webrtc::SetRemoteDescriptionObserverInterface {
   public:
    SetRemoteObserver(webrtc_engine* engine, bool is_offer)
        : engine_(engine), is_offer_(is_offer) {}
    void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override;

   private:
    webrtc_engine* engine_;
    bool is_offer_;
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
  void CreateOfferIceRestart();   // 同 PC 上 ICE 重启（复用旧候选，新 ufrag）
  void ReconnectImpl();           // reconnect 命令的 signaling 线程实现
  void ReleasePeerConnection();   // 只拆 PC/DC，保留 factory 与通话身份
  void RebuildAndOffer();         // 释放旧 PC 后按本端 mode 重建并发起新 offer（命令方对称）
  void SetRemoteOffer(const std::string& sdp);
  void SetRemoteAnswer(const std::string& sdp);
  void AddRemoteCandidate(const std::string& candidate,
                          const std::string& sdp_mid, int sdp_mline_index);
  void ClosePeerConnection();
  void RequestStatsImpl();
  void CollectDataChannelStatsImpl(EngineStats::DataChannelInfo* out);
  // 视频（第一阶段：合成测试图源）
  void StartVideoImpl();
  void StopVideoImpl();
  void VideoOnImpl();
  void VideoOffImpl();
  void SetBitrateImpl(int max_kbps);
  void SetFramerateImpl(int fps);
  void SetDegradationImpl(const std::string& mode);
  void GetVideoStatusImpl();
  void TriggerRenegotiation();      // 命令方直接 CreateOffer（对称重协商）
  void CreateVideoTrack();          // 懒创建合成源 + VideoTrack
  bool AddVideoTrackToPc();         // AddTrack 到当前 PC（返回成败）
  bool RemoveVideoTrackFromPc();    // RemoveTrack + 释放 track/sender（返回成败）
  bool ApplyRtpParameters();        // 用当前 video_params_ 调 video_sender_->SetParameters

  std::unique_ptr<webrtc::Thread> signaling_thread_;
  std::unique_ptr<webrtc::Thread> worker_thread_;
  std::unique_ptr<webrtc::Thread> network_thread_;

  WebSocketSignalingClient signaling_client_;

  std::optional<webrtc::Environment> environment_;  // 供合成视频源取 task_queue_factory

  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface>
      peer_connection_factory_;
  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection_;
  webrtc::scoped_refptr<webrtc::DataChannelInterface> data_channel_;
  webrtc::PeerConnectionInterface::RTCConfiguration rtc_configuration_;
  IceConfig ice_config_;  // 当前 ICE 配置（mode 可变，server 固定）
  webrtc::scoped_refptr<CreateSdpObserver> sdp_observer_;

  // 视频（第一阶段）
  bool video_enabled_ = false;
  bool video_switch_in_progress_ = false;  // video_on/off 重协商进行中
  webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> video_source_;
  webrtc::scoped_refptr<webrtc::VideoTrackInterface> video_track_;
  webrtc::scoped_refptr<webrtc::RtpSenderInterface> video_sender_;
  std::unique_ptr<webrtc::VideoSinkInterface<webrtc::VideoFrame>> remote_video_sink_;
  // 当前视频参数（供 get_video_status 回显 + SetParameters 应用）
  int video_width_ = 640;
  int video_height_ = 480;
  int video_fps_ = 30;
  int video_bitrate_kbps_ = 0;   // 0 = 不设上限
  std::string video_degradation_ = "balanced";

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
  bool reconnect_in_progress_ = false;   // 正在 ICE 重连（复用旧候选）
  bool renegotiating_ = false;            // 本轮 CreateOffer 是重建 reoffer 而非首次 call
  std::string last_received_message_;
};

#endif  // APPS_PEERCONNECTION_WEBRTC_CLIENT_WEBRTC_ENGINE_H_
