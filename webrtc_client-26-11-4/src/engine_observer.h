#ifndef APPS_PEERCONNECTION_WEBRTC_CLIENT_ENGINE_OBSERVER_H_
#define APPS_PEERCONNECTION_WEBRTC_CLIENT_ENGINE_OBSERVER_H_

#include <cstdint>
#include <string>
#include <vector>

#include "api/data_channel_interface.h"
#include "api/peer_connection_interface.h"
#include "signaling/ws_signaling_client.h"

// 引擎采集的运行时状态快照：DataChannel 同步信息 + PeerConnection GetStats。
// 「采集不到」时字段为空/0（空 state 或全 0 表示对应对象尚未建立）。
struct EngineStats {
  // ===== DataChannel 信息（来自 DataChannelInterface 的同步 getter）=====
  struct DataChannelInfo {
    std::string state;  // 通道状态：connecting(连接中)/open(可用)/closing(关闭中)/closed(已关闭)
    std::string label;  // 通道标签（本客户端创建时固定为 "chat"）
    std::string protocol;  // 子协议名（DataChannelInit.protocol，默认空）
    bool reliable = false;  // 是否可靠传输（SCTP 带重传，默认 true）
    bool ordered = false;   // 是否保证有序交付（默认 true）
    int id = -1;            // SCTP 流 id（-1 表示带内协商中，尚未确定）
    uint32_t messages_sent = 0;      // 已发送的应用层消息条数
    uint64_t bytes_sent = 0;         // 已发送的应用层字节数（不含 SCTP/DTLS 头）
    uint32_t messages_received = 0;  // 已接收的应用层消息条数
    uint64_t bytes_received = 0;     // 已接收的应用层字节数
    uint64_t buffered_amount = 0;    // 已排队但还没发完的字节数（SCTP 发送队列积压，拥塞指标：持续增长说明发送快于链路消化）
  };

  // ===== 传输层信息（来自 GetStats 的 RTCTransportStats：SCTP/DTLS/ICE 总账）=====
  struct TransportInfo {
    uint64_t bytes_sent = 0;      // 传输层已发字节（含 SCTP 封装 + DTLS 加密头，故 > data_channel.bytes_sent）
    uint64_t packets_sent = 0;    // 传输层已发包数
    uint64_t bytes_received = 0;  // 传输层已收字节
    uint64_t packets_received = 0;  // 传输层已收包数
    std::string dtls_state;       // DTLS 握手状态：new/connecting/connected/closed/failed
    std::string ice_state;        // ICE 状态：new/checking/connected/completed/disconnected/failed/closed
    std::string ice_role;         // ICE 角色：controlling(主动方)/controlled(被动方)
    std::string selected_candidate_pair_id;  // 当前选中候选对的 id（关联 candidate_pair）
  };

  // ===== 单个 ICE 候选地址（本端/对端的网络端点，来自 RTCIceCandidateStats）=====
  struct CandidateInfo {
    std::string ip;             // 该候选的 IP 地址
    int port = 0;               // 该候选的端口号
    std::string protocol;       // 传输协议：udp / tcp
    std::string candidate_type; // 候选类型：host(直连本机地址) / srflx(STUN 反射地址) / relay(TURN 中继地址) / prflx(对端反射地址)
  };

  // ===== 候选对信息（来自 GetStats 的 RTCIceCandidatePairStats：实际承载数据的那条网络路径）=====
  struct CandidatePairInfo {
    std::string state;          // 候选对状态：frozen/waiting/in-progress/succeeded/failed
    bool nominated = false;     // 是否被提名（即被选中用于实际传输）
    bool writable = false;      // 是否可写（连通性检查通过、当前能发包）
    uint64_t packets_sent = 0;      // 该路径已发包数
    uint64_t packets_received = 0;  // 该路径已收包数
    uint64_t bytes_sent = 0;        // 该路径已发字节
    uint64_t bytes_received = 0;    // 该路径已收字节
    double current_round_trip_time = 0.0;  // 当前 RTT（秒）——通信延迟的核心指标；刚建立时可能为 0，需一次测量后才有值
    double total_round_trip_time = 0.0;    // 累计 RTT 总和（秒）
    double available_outgoing_bitrate = 0.0;  // 可用上行带宽估计（bps）；仅当有音视频 RTP track 时非 0，纯 DataChannel 恒为 0
    double available_incoming_bitrate = 0.0;  // 可用下行带宽估计（bps）；同上
    CandidateInfo local_candidate;   // 本端地址（ip/port/protocol/type）
    CandidateInfo remote_candidate;  // 对端地址（ip/port/protocol/type）
  };

  // ===== 音视频 RTP 流统计（字段预留：当前无音视频 track，恒为空/0；接入音视频后由 StatsCollector 填充）=====

  // 本端「接收」的一路媒体流（来自 GetStats 的 RTCInboundRtpStreamStats）。
  struct InboundRtpInfo {
    std::string kind;    // 媒体类型：audio / video
    std::string codec;   // 编解码器（由 codec_id 解析成 mime_type，如 video/VP8、audio/opus）
    uint32_t ssrc = 0;   // RTP 同步源标识
    uint32_t packets_received = 0;  // 已接收 RTP 包数
    uint64_t bytes_received = 0;    // 已接收字节数
    int32_t packets_lost = 0;       // 累计丢包数（RFC 3550，可为负表示重复/乱序修正）
    double jitter = 0.0;            // 抖动（秒）——音频卡顿/爆音的核心指标
    double frames_per_second = 0.0; // 视频帧率（仅视频）
    uint32_t frame_width = 0;       // 视频宽度（仅视频）
    uint32_t frame_height = 0;      // 视频高度（仅视频）
    uint32_t frames_decoded = 0;    // 已解码帧数（仅视频）
    uint32_t frames_dropped = 0;    // 已丢弃帧数（解码端扛不住的信号，仅视频）
    double audio_level = 0.0;       // 接收音频电平（仅音频）
  };

  // 本端「发送」的一路媒体流（来自 RTCInboundRtpStreamStats）。
  struct OutboundRtpInfo {
    std::string kind;    // 媒体类型：audio / video
    std::string codec;   // 编解码器（mime_type）
    uint32_t ssrc = 0;   // RTP 同步源标识
    uint32_t packets_sent = 0;      // 已发送 RTP 包数
    uint64_t bytes_sent = 0;        // 已发送字节数
    double target_bitrate = 0.0;    // 目标码率（bps，视频编码目标）
    uint32_t frames_encoded = 0;    // 已编码帧数（仅视频）
    double frames_per_second = 0.0; // 编码帧率（仅视频）
    uint32_t frame_width = 0;       // 发送视频宽度（仅视频）
    uint32_t frame_height = 0;      // 发送视频高度（仅视频）
    std::string quality_limitation_reason;  // 视频质量受限原因：none/cpu/bandwidth/other（空=不适用）
  };

  // 对端「报告回来」的接收质量（来自 RTCRemoteInboundRtpStreamStats：我们发的流在对方那边的表现）。
  struct RemoteInboundRtpInfo {
    std::string kind;    // 媒体类型：audio / video
    uint32_t ssrc = 0;   // RTP 同步源标识
    double round_trip_time = 0.0;   // RTP 层 RTT（秒）
    double fraction_lost = 0.0;     // 丢包率（0~1，如 0.03 表示 3%）
    double jitter = 0.0;            // 抖动（秒）
  };

  DataChannelInfo data_channel;        // 数据通道快照
  TransportInfo transport;             // 传输层快照
  CandidatePairInfo candidate_pair;    // 活跃候选对（RTT + 两端地址）
  std::vector<InboundRtpInfo> inbound_rtp;          // 接收的媒体流（0~N 路，音频/视频各一路）
  std::vector<OutboundRtpInfo> outbound_rtp;        // 发送的媒体流（0~N 路）
  std::vector<RemoteInboundRtpInfo> remote_inbound_rtp;  // 对端反馈（每路发送流对应一条）
  uint32_t data_channels_opened = 0;   // 累计打开过的 DataChannel 数量
  uint32_t data_channels_closed = 0;   // 累计关闭过的 DataChannel 数量
};

// 引擎对外事件接口：上层（IPC/UI）实现，接收信令、SDP/ICE、PeerConnection、
// DataChannel 的状态与消息。所有回调在引擎的 signaling 线程同步触发。
struct EngineObserver {
  virtual ~EngineObserver() = default;

  // signal_client
  virtual void OnSignalSignedIn(int my_id) = 0;
  virtual void OnSignalDisconnected() = 0;
  virtual void OnSignalRoster(const std::vector<PeerInfo>& peers) = 0;
  virtual void OnSignalCall(int from) = 0;
  virtual void OnSignalMessage(int from, const std::string& text) = 0;
  virtual void OnCallEnded(int from) = 0;
  // 信令层错误：信令服务器返回（offline/busy/name_taken）或连接失败，对应 signal_client.error。
  virtual void OnSignalError(const std::string& code, const std::string& msg) = 0;
  // 引擎内部异步失败：factory/PC/DC/SDP/SetLocal/SetRemote 操作失败，对应 engine.error。
  virtual void OnEngineError(const std::string& code, const std::string& msg) = 0;

  // SDP / ICE（透明信令）
  virtual void OnLocalSdp(const std::string& type, const std::string& sdp) = 0;
  virtual void OnRemoteSdp(const std::string& type, const std::string& sdp) = 0;
  virtual void OnLocalCandidate(const std::string& candidate,
                                const std::string& sdp_mid,
                                int sdp_mline_index) = 0;
  virtual void OnRemoteCandidate(const std::string& candidate,
                                 const std::string& sdp_mid,
                                 int sdp_mline_index) = 0;

  // peer_connection
  virtual void OnPeerConnectionStateChange(
      webrtc::PeerConnectionInterface::PeerConnectionState state) = 0;

  // data_channel
  virtual void OnDataChannelStateChange(
      webrtc::DataChannelInterface::DataState state) = 0;
  virtual void OnDataChannelMessage(const std::string& message) = 0;

  // 运行时状态快照（get_stats 命令触发）。
  virtual void OnStats(const EngineStats& stats) = 0;
};

#endif  // APPS_PEERCONNECTION_WEBRTC_CLIENT_ENGINE_OBSERVER_H_
