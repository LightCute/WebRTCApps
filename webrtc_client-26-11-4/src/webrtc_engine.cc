#include "webrtc_engine.h"

#include <map>

#include "api/create_modular_peer_connection_factory.h"
#include "api/environment/environment.h"
#include "api/environment/environment_factory.h"
#include "api/field_trials.h"
#include "api/stats/rtcstats_objects.h"
#include "rtc_base/logging.h"
#include "rtc_base/ssl_adapter.h"

using namespace webrtc;

// ============================================================================
//  构造 / 析构
// ============================================================================

webrtc_engine::webrtc_engine() {
  rtc_configuration_.sdp_semantics = SdpSemantics::kUnifiedPlan;
  sdp_observer_ = make_ref_counted<CreateSdpObserver>(this);
}

webrtc_engine::~webrtc_engine() {
  DisconnectFromServer();
  Stop();
}

// ============================================================================
//  生命周期：Start / Stop（经典三线程）
// ============================================================================

bool webrtc_engine::Start() {
  if (is_running()) {
    return true;
  }

  signaling_thread_ = webrtc::Thread::CreateWithSocketServer();
  worker_thread_ = webrtc::Thread::Create();
  network_thread_ = webrtc::Thread::CreateWithSocketServer();

  signaling_thread_->SetName("webrtc_engine_signaling", nullptr);
  worker_thread_->SetName("webrtc_engine_worker", nullptr);
  network_thread_->SetName("webrtc_engine_network", nullptr);

  bool signaling_ok = signaling_thread_->Start();
  bool worker_ok = worker_thread_->Start();
  bool network_ok = network_thread_->Start();
  if (!signaling_ok || !worker_ok || !network_ok) {
    Stop();
    return false;
  }
  return true;
}

void webrtc_engine::Stop() {
  // 先清理 WebRTC 对象（PC/DC/factory 必须在 signaling 线程销毁），再停线程。
  if (signaling_thread_) {
    signaling_thread_->BlockingCall([this] { ClosePeerConnection(); });
  } else {
    ClosePeerConnection();
  }

  if (network_thread_) {
    network_thread_->Stop();
    network_thread_.reset();
  }
  if (worker_thread_) {
    worker_thread_->Stop();
    worker_thread_.reset();
  }
  if (signaling_thread_) {
    signaling_thread_->Stop();
    signaling_thread_.reset();
  }
}

// ============================================================================
//  信令连接管理
// ============================================================================

void webrtc_engine::ConnectToServer(const std::string& server, int port,
                                    const std::string& client_name) {
  if (!is_running()) {
    RTC_LOG(LS_WARNING) << "ConnectToServer: engine not started";
    return;
  }
  signaling_client_.RegisterObserver(this);
  signaling_client_.SetCallbackExecutor([this](std::function<void()> fn) {
    if (signaling_thread_) {
      signaling_thread_->PostTask(std::move(fn));
    }
  });
  signaling_client_.Connect(server, port, client_name);
}

void webrtc_engine::DisconnectFromServer() {
  signaling_client_.Close();
}

void webrtc_engine::DisconnectSignal() {
  signaling_client_.Disconnect();  // 软断开：保留 io 线程，可重连
}

void webrtc_engine::RegisterObserver(EngineObserver* observer) {
  engine_observer_ = observer;
}

void webrtc_engine::ConfigureIce(const IceConfig& cfg) {
  ice_config_ = cfg;
  rtc_configuration_.servers.clear();

  auto add_stun = [&]() {
    if (cfg.stun_uri.empty()) return;
    PeerConnectionInterface::IceServer s;
    s.uri = cfg.stun_uri;
    rtc_configuration_.servers.push_back(s);
  };
  auto add_turn = [&]() {
    if (cfg.turn_uri.empty()) return;
    PeerConnectionInterface::IceServer t;
    t.uri = cfg.turn_uri;
    t.username = cfg.turn_user;
    t.password = cfg.turn_pass;
    rtc_configuration_.servers.push_back(t);
  };

  switch (cfg.mode) {
    case IceTransportsMode::kHost:
      // 不加任何 server → 只收集 host 候选。
      rtc_configuration_.type =
          PeerConnectionInterface::IceTransportsType::kAll;
      break;
    case IceTransportsMode::kSrflx:
      // 屏蔽 host，只留 STUN 反射（不配 TURN 就只有 srflx）。
      rtc_configuration_.type =
          PeerConnectionInterface::IceTransportsType::kNoHost;
      add_stun();
      break;
    case IceTransportsMode::kRelay:
      rtc_configuration_.type =
          PeerConnectionInterface::IceTransportsType::kRelay;
      add_turn();
      break;
    case IceTransportsMode::kAuto:
    default:
      rtc_configuration_.type =
          PeerConnectionInterface::IceTransportsType::kAll;
      add_stun();
      add_turn();
      break;
  }

  // 限制本机 ICE 端口范围（0 = 不限制），配合云安全组放行固定端口段。
  rtc_configuration_.set_min_port(cfg.min_port);
  rtc_configuration_.set_max_port(cfg.max_port);
}

// 运行时切换 ICE 模式（空闲时用；须在 signaling 线程调用）。
void webrtc_engine::SetIceTransports(const std::string& mode) {
  ice_config_.mode = ParseIceTransports(mode);
  ConfigureIce(ice_config_);
  RTC_LOG(LS_INFO) << "SetIceTransports mode=" << mode
                   << " type=" << static_cast<int>(rtc_configuration_.type)
                   << " servers=" << rtc_configuration_.servers.size();
}

// 通话中切换本端 ICE 模式 + 重建 PC 重新连接（须在 signaling 线程调用）。
void webrtc_engine::SwitchIce(const std::string& mode) {
  SetIceTransports(mode);  // 只改本端 mode
  RTC_LOG(LS_INFO) << "SwitchIce mode=" << mode
                   << " type=" << static_cast<int>(rtc_configuration_.type)
                   << " servers=" << rtc_configuration_.servers.size();
  ice_switch_in_progress_ = true;
  signaling_client_.SendSwitchIce(peer_id_);
  if (is_offerer_) {
    ReconnectAsOfferer();
  } else {
    ReleasePeerConnection();  // 释放旧 PC（旧 TURN 分配），等对端 reoffer
  }
}

// 通话中手动触发 ICE 重启（复用旧候选，不重建 PC、不改 mode）。
void webrtc_engine::Reconnect() {
  if (!signaling_thread_) {
    return;
  }
  if (signaling_thread_->IsCurrent()) {
    ReconnectImpl();
    return;
  }
  signaling_thread_->BlockingCall([&] { ReconnectImpl(); });
}

void webrtc_engine::ReconnectImpl() {
  RTC_LOG(LS_INFO) << "Reconnect";
  reconnect_in_progress_ = true;
  if (is_offerer_) {
    CreateOfferIceRestart();
  } else {
    signaling_client_.SendReconnect(peer_id_);  // 请 offerer 发起 ICE 重启
  }
}

// ============================================================================
//  公共 API（BlockingCall marshal 到 signaling 线程）
// ============================================================================

bool webrtc_engine::InitializePeerConnectionFactory() {
  if (!signaling_thread_) {
    return false;
  }
  if (signaling_thread_->IsCurrent()) {
    return InitializePeerConnectionFactoryImpl();
  }
  bool ok = false;
  signaling_thread_->BlockingCall(
      [&] { ok = InitializePeerConnectionFactoryImpl(); });
  return ok;
}

bool webrtc_engine::CreatePeerConnection() {
  if (!signaling_thread_) {
    return false;
  }
  if (signaling_thread_->IsCurrent()) {
    return CreatePeerConnectionImpl();
  }
  bool ok = false;
  signaling_thread_->BlockingCall([&] { ok = CreatePeerConnectionImpl(); });
  return ok;
}

bool webrtc_engine::CreateDataChannel() {
  if (!signaling_thread_) {
    return false;
  }
  if (signaling_thread_->IsCurrent()) {
    return CreateDataChannelImpl();
  }
  bool ok = false;
  signaling_thread_->BlockingCall([&] { ok = CreateDataChannelImpl(); });
  return ok;
}

void webrtc_engine::StartCall(int peer_id) {
  if (!signaling_thread_) {
    RTC_LOG(LS_WARNING) << "StartCall: engine not started";
    return;
  }
  if (signaling_thread_->IsCurrent()) {
    StartCallImpl(peer_id);
    return;
  }
  signaling_thread_->BlockingCall([&] { StartCallImpl(peer_id); });
}

void webrtc_engine::Leave() {
  signaling_client_.Leave();  // 发送线程安全，无需 marshal
}

void webrtc_engine::GetRoster() {
  signaling_client_.GetRoster();  // 发送线程安全，无需 marshal
}

bool webrtc_engine::SendDataChannelMessage(const std::string& message) {
  if (!signaling_thread_) {
    return false;
  }
  if (signaling_thread_->IsCurrent()) {
    return SendDataChannelMessageImpl(message);
  }
  bool ok = false;
  signaling_thread_->BlockingCall(
      [&] { ok = SendDataChannelMessageImpl(message); });
  return ok;
}

bool webrtc_engine::SendSignalMessage(int peer_id, const std::string& text) {
  return signaling_client_.SendMessage(peer_id, text);  // 发送线程安全，无需 marshal
}

void webrtc_engine::RequestStats() {
  if (!signaling_thread_) {
    return;
  }
  if (signaling_thread_->IsCurrent()) {
    RequestStatsImpl();
    return;
  }
  signaling_thread_->PostTask([this] { RequestStatsImpl(); });
}

// ============================================================================
//  内部实现（假定已在 signaling 线程）
// ============================================================================

bool webrtc_engine::InitializePeerConnectionFactoryImpl() {
  if (peer_connection_factory_) {
    return true;
  }
  InitializeSSL();
  Environment environment =
      CreateEnvironment(std::make_unique<FieldTrials>(""));
  PeerConnectionFactoryDependencies deps;
  deps.signaling_thread = signaling_thread_.get();
  deps.network_thread = network_thread_.get();
  deps.worker_thread = worker_thread_.get();
  deps.env = environment;
  deps.adm = nullptr;
  peer_connection_factory_ =
      CreateModularPeerConnectionFactory(std::move(deps));
  if (!peer_connection_factory_) {
    RTC_LOG(LS_ERROR) << "CreateModularPeerConnectionFactory failed";
    if (engine_observer_) engine_observer_->OnEngineError("peer_connection_factory_failed",
                      "CreateModularPeerConnectionFactory failed");
    return false;
  }
  RTC_LOG(LS_INFO) << "PeerConnectionFactory created";
  return true;
}

bool webrtc_engine::CreatePeerConnectionImpl() {
  if (peer_connection_) {
    return true;
  }
  if (!peer_connection_factory_) {
    RTC_LOG(LS_WARNING) << "CreatePeerConnection: factory not created";
    if (engine_observer_) engine_observer_->OnEngineError("peer_connection_failed", "factory not created");
    return false;
  }
  PeerConnectionDependencies pc_deps(this);
  auto result = peer_connection_factory_->CreatePeerConnectionOrError(
      rtc_configuration_, std::move(pc_deps));
  if (!result.ok()) {
    RTC_LOG(LS_ERROR) << "CreatePeerConnection failed: "
                      << result.error().message();
    if (engine_observer_) engine_observer_->OnEngineError("peer_connection_failed", result.error().message());
    return false;
  }
  peer_connection_ = std::move(result.value());
  RTC_LOG(LS_INFO) << "PeerConnection created";
  return true;
}

bool webrtc_engine::CreateDataChannelImpl() {
  if (data_channel_) {
    return true;
  }
  if (!peer_connection_) {
    RTC_LOG(LS_WARNING) << "CreateDataChannel: PeerConnection not created";
    if (engine_observer_) engine_observer_->OnEngineError("data_channel_failed", "PeerConnection not created");
    return false;
  }
  DataChannelInit init;
  init.negotiated = false;
  init.id = 0;
  auto result = peer_connection_->CreateDataChannelOrError("chat", &init);
  if (!result.ok()) {
    RTC_LOG(LS_ERROR) << "CreateDataChannel failed: "
                      << result.error().message();
    if (engine_observer_) engine_observer_->OnEngineError("data_channel_failed", result.error().message());
    return false;
  }
  data_channel_ = std::move(result.value());
  data_channel_->RegisterObserver(this);
  RTC_LOG(LS_INFO) << "DataChannel created";
  return true;
}

void webrtc_engine::StartCallImpl(int peer_id) {
  peer_id_ = peer_id;
  is_offerer_ = true;
  renegotiating_ = false;
  if (!InitializePeerConnectionFactoryImpl()) {
    return;
  }
  if (!CreatePeerConnectionImpl()) {
    return;
  }
  if (!CreateDataChannelImpl()) {
    return;
  }
  CreateOffer();
}

bool webrtc_engine::SendDataChannelMessageImpl(const std::string& message) {
  if (!data_channel_ ||
      data_channel_->state() != DataChannelInterface::kOpen) {
    return false;
  }
  return data_channel_->Send(DataBuffer(message));
}

void webrtc_engine::CreateOffer() {
  PeerConnectionInterface::RTCOfferAnswerOptions options;
  peer_connection_->CreateOffer(sdp_observer_.get(), options);
}

void webrtc_engine::CreateAnswer() {
  PeerConnectionInterface::RTCOfferAnswerOptions options;
  peer_connection_->CreateAnswer(sdp_observer_.get(), options);
}

void webrtc_engine::CreateOfferIceRestart() {
  renegotiating_ = true;  // 使 OnSuccess 走 reoffer 而非首次 call
  PeerConnectionInterface::RTCOfferAnswerOptions options;
  options.ice_restart = true;
  peer_connection_->CreateOffer(sdp_observer_.get(), options);
}

void webrtc_engine::SetRemoteOffer(const std::string& sdp) {
  auto desc = CreateSessionDescription(SdpType::kOffer, sdp);
  if (!desc) {
    RTC_LOG(LS_ERROR) << "Failed to parse remote offer";
    if (engine_observer_) engine_observer_->OnEngineError("set_remote_desc_failed", "failed to parse remote offer");
    return;
  }
  peer_connection_->SetRemoteDescription(
      std::move(desc), make_ref_counted<SetRemoteObserver>(this));
}

void webrtc_engine::SetRemoteAnswer(const std::string& sdp) {
  auto desc = CreateSessionDescription(SdpType::kAnswer, sdp);
  if (!desc) {
    RTC_LOG(LS_ERROR) << "Failed to parse remote answer";
    if (engine_observer_) engine_observer_->OnEngineError("set_remote_desc_failed", "failed to parse remote answer");
    return;
  }
  peer_connection_->SetRemoteDescription(
      std::move(desc), make_ref_counted<SetRemoteObserver>(this));
}

void webrtc_engine::AddRemoteCandidate(const std::string& candidate,
                                       const std::string& sdp_mid,
                                       int sdp_mline_index) {
  SdpParseError error;
  IceCandidateInterface* ice =
      CreateIceCandidate(sdp_mid, sdp_mline_index, candidate, &error);
  if (!ice) {
    RTC_LOG(LS_WARNING) << "Failed to parse candidate: " << error.description;
    return;
  }
  peer_connection_->AddIceCandidate(
      std::unique_ptr<IceCandidateInterface>(ice), [](RTCError err) {
        if (!err.ok()) {
          RTC_LOG(LS_WARNING) << "AddIceCandidate failed: " << err.message();
        }
      });
}

void webrtc_engine::ClosePeerConnection() {
  data_channel_ = nullptr;
  if (peer_connection_) {
    peer_connection_->Close();
    peer_connection_ = nullptr;
  }
  peer_connection_factory_ = nullptr;
  peer_id_ = -1;
  is_offerer_ = false;
  is_connected_ = false;
  data_channel_open_ = false;
}

// 只拆 PC/DC，保留 factory 与 peer_id_/is_offerer_（切换 mode 时重建用）。
void webrtc_engine::ReleasePeerConnection() {
  data_channel_ = nullptr;
  if (peer_connection_) {
    peer_connection_->Close();
    peer_connection_ = nullptr;
  }
  is_connected_ = false;
  data_channel_open_ = false;
}

// 释放旧 PC 后按本端当前 mode 重建 PC/DC 并发起新 offer（offerer 用）。
void webrtc_engine::ReconnectAsOfferer() {
  renegotiating_ = true;
  ReleasePeerConnection();
  if (!CreatePeerConnectionImpl()) {
    if (engine_observer_) engine_observer_->OnIceSwitched(false);
    signaling_client_.Leave();
    return;
  }
  if (!CreateDataChannelImpl()) {
    if (engine_observer_) engine_observer_->OnIceSwitched(false);
    signaling_client_.Leave();
    return;
  }
  CreateOffer();
}

void webrtc_engine::RequestStatsImpl() {
  EngineStats stats;
  CollectDataChannelStatsImpl(&stats.data_channel);
  if (peer_connection_) {
    scoped_refptr<StatsCollector> collector =
        make_ref_counted<StatsCollector>(this, std::move(stats));
    // GetStats 内部会把裸指针包成 scoped_refptr 持有，回调期间保持存活。
    peer_connection_->GetStats(collector.get());
  } else if (engine_observer_) {
    engine_observer_->OnStats(stats);  // 无 PC：只上抛 DataChannel 信息（可能全空）
  }
}

void webrtc_engine::CollectDataChannelStatsImpl(
    EngineStats::DataChannelInfo* out) {
  if (!data_channel_ || !out) {
    return;
  }
  out->state = DataChannelInterface::DataStateString(data_channel_->state());
  out->label = data_channel_->label();
  out->protocol = data_channel_->protocol();
  out->reliable = data_channel_->reliable();
  out->ordered = data_channel_->ordered();
  out->id = data_channel_->id();
  out->messages_sent = data_channel_->messages_sent();
  out->bytes_sent = data_channel_->bytes_sent();
  out->messages_received = data_channel_->messages_received();
  out->bytes_received = data_channel_->bytes_received();
  out->buffered_amount = data_channel_->buffered_amount();
}

// ============================================================================
//  观察者实现 —— SignalingClientObserver（signaling 线程，经 executor marshal）
// ============================================================================

void webrtc_engine::OnSignalSignedIn(int my_id) {
  callback_on_signaling_thread_ =
      (webrtc::Thread::Current() == signaling_thread_.get());
  my_id_ = my_id;
  signed_in_ = true;
  RTC_LOG(LS_INFO) << "OnSignalSignedIn my_id=" << my_id;
  if (engine_observer_) {
    engine_observer_->OnSignalSignedIn(my_id);
  }
}

void webrtc_engine::OnSignalDisconnected() {
  signed_in_ = false;
  my_id_ = -1;
  RTC_LOG(LS_INFO) << "OnSignalDisconnected";
  if (engine_observer_) {
    engine_observer_->OnSignalDisconnected();
  }
  ClosePeerConnection();  // 信令断开 → 拆 PC/DC（不发 leave，信令已不通）
}

void webrtc_engine::OnSignalRoster(const std::vector<PeerInfo>& peers) {
  peers_ = peers;
  RTC_LOG(LS_INFO) << "OnSignalRoster peers=" << peers.size();
  if (engine_observer_) {
    engine_observer_->OnSignalRoster(peers);
  }
}

void webrtc_engine::OnSignalCall(int from, const std::string& sdp) {
  RTC_LOG(LS_INFO) << "OnSignalCall from=" << from;
  peer_id_ = from;
  is_offerer_ = false;
  renegotiating_ = false;
  if (engine_observer_) {
    engine_observer_->OnSignalCall(from);
    engine_observer_->OnRemoteSdp("offer", sdp);
  }
  if (!InitializePeerConnectionFactoryImpl()) {
    return;
  }
  if (!CreatePeerConnectionImpl()) {
    return;
  }
  SetRemoteOffer(sdp);
}

void webrtc_engine::OnSignalAnswer(int from, const std::string& sdp) {
  RTC_LOG(LS_INFO) << "OnSignalAnswer from=" << from;
  if (engine_observer_) {
    engine_observer_->OnRemoteSdp("answer", sdp);
  }
  SetRemoteAnswer(sdp);
}

void webrtc_engine::OnSignalCandidate(int from, const std::string& candidate,
                                const std::string& sdp_mid,
                                int sdp_mline_index) {
  RTC_LOG(LS_INFO) << "OnSignalCandidate from=" << from;
  if (engine_observer_) {
    engine_observer_->OnRemoteCandidate(candidate, sdp_mid, sdp_mline_index);
  }
  AddRemoteCandidate(candidate, sdp_mid, sdp_mline_index);
}

void webrtc_engine::OnSignalLeave(int from) {
  RTC_LOG(LS_INFO) << "OnSignalLeave from=" << from;
  ClosePeerConnection();
  if (engine_observer_) {
    engine_observer_->OnCallEnded(from);
  }
}

void webrtc_engine::OnSignalPeerGone(int from) {
  RTC_LOG(LS_INFO) << "OnSignalPeerGone from=" << from;
  ClosePeerConnection();
  if (engine_observer_) {
    engine_observer_->OnCallEnded(from);
  }
}

void webrtc_engine::OnSignalMessage(int from, const std::string& text) {
  RTC_LOG(LS_INFO) << "OnSignalMessage from=" << from;
  if (engine_observer_) {
    engine_observer_->OnSignalMessage(from, text);
  }
}

void webrtc_engine::OnSignalError(const std::string& code, const std::string& msg) {
  RTC_LOG(LS_WARNING) << "OnSignalError code=" << code << " msg=" << msg;
  if (engine_observer_) {
    engine_observer_->OnSignalError(code, msg);
  }
  // 呼叫被拒（对端离线/忙线）：清理半开的 PC、复位 peer_id_，避免卡在 in-call 状态。
  if ((code == "peer_offline" || code == "peer_busy") && peer_id_ > 0) {
    ClosePeerConnection();
  }
}

void webrtc_engine::OnSignalSwitchIce(int from) {
  RTC_LOG(LS_INFO) << "OnSignalSwitchIce from=" << from;
  // 对端要切模式：本端 mode 不变，只重建自己的 PC。
  ice_switch_in_progress_ = true;
  if (is_offerer_) {
    ReconnectAsOfferer();
  } else {
    ReleasePeerConnection();  // 释放旧 PC（旧 TURN 分配），等 reoffer
  }
  if (engine_observer_) {
    engine_observer_->OnSignalSwitchIce(from);
  }
}

void webrtc_engine::OnSignalReconnect(int from) {
  RTC_LOG(LS_INFO) << "OnSignalReconnect from=" << from;
  reconnect_in_progress_ = true;
  if (is_offerer_) {
    CreateOfferIceRestart();  // offerer 收到对端请求后发起 ICE 重启
  }
  if (engine_observer_) {
    engine_observer_->OnSignalReconnect(from);
  }
}

void webrtc_engine::OnSignalReoffer(int from, const std::string& sdp) {
  RTC_LOG(LS_INFO) << "OnSignalReoffer from=" << from;
  if (from != peer_id_) {
    return;  // 防御：非当前通话对端的重建 offer 忽略
  }
  if (!peer_connection_) {
    if (!InitializePeerConnectionFactoryImpl()) {
      return;
    }
    if (!CreatePeerConnectionImpl()) {
      return;
    }
  }
  if (engine_observer_) {
    engine_observer_->OnRemoteSdp("offer", sdp);
  }
  SetRemoteOffer(sdp);  // → CreateAnswer → 回 answer
}

// ============================================================================
//  观察者实现 —— PeerConnectionObserver（signaling 线程）
// ============================================================================

void webrtc_engine::OnSignalingChange(
    PeerConnectionInterface::SignalingState new_state) {
  RTC_LOG(LS_INFO) << "SignalingState: "
                   << PeerConnectionInterface::AsString(new_state);
}

void webrtc_engine::OnDataChannel(
    scoped_refptr<DataChannelInterface> data_channel) {
  RTC_LOG(LS_INFO) << "OnDataChannel";
  data_channel_ = data_channel;
  data_channel_->RegisterObserver(this);
}

void webrtc_engine::OnIceGatheringChange(
    PeerConnectionInterface::IceGatheringState new_state) {
  RTC_LOG(LS_INFO) << "IceGatheringState: "
                   << PeerConnectionInterface::AsString(new_state);
}

void webrtc_engine::OnIceConnectionChange(
    PeerConnectionInterface::IceConnectionState new_state) {
  RTC_LOG(LS_INFO) << "IceConnectionState: " << static_cast<int>(new_state);
}

void webrtc_engine::OnIceCandidate(const IceCandidate* candidate) {
  RTC_LOG(LS_INFO) << "OnIceCandidate: " << candidate->candidate().ToString();
  if (peer_id_ < 0) {
    return;
  }
  std::string candidate_str;
  candidate->ToString(&candidate_str);
  if (engine_observer_) {
    engine_observer_->OnLocalCandidate(candidate_str, candidate->sdp_mid(),
                                candidate->sdp_mline_index());
  }
  signaling_client_.Candidate(peer_id_, candidate_str, candidate->sdp_mid(),
                              candidate->sdp_mline_index());
}

void webrtc_engine::OnConnectionChange(
    PeerConnectionInterface::PeerConnectionState state) {
  RTC_LOG(LS_INFO) << "PeerConnectionState: "
                   << PeerConnectionInterface::AsString(state);
  is_connected_ = (state == PeerConnectionInterface::PeerConnectionState::kConnected);
  if (engine_observer_) {
    engine_observer_->OnPeerConnectionStateChange(state);
  }

  // ICE 模式切换结果检测
  if (ice_switch_in_progress_) {
    if (state == PeerConnectionInterface::PeerConnectionState::kConnected) {
      ice_switch_in_progress_ = false;
      if (engine_observer_) engine_observer_->OnIceSwitched(true);
    } else if (state == PeerConnectionInterface::PeerConnectionState::kFailed) {
      ice_switch_in_progress_ = false;
      if (engine_observer_) engine_observer_->OnIceSwitched(false);
    }
  }

  // ICE/DTLS 彻底失败：主动挂断，让信令服务器清掉 busy 状态、双方清理通话。
  // kDisconnected 是临时抖动（交给 ICE 自动恢复）；kClosed 是正常关闭，都不处理。
  if (state == PeerConnectionInterface::PeerConnectionState::kFailed &&
      peer_id_ > 0) {
    signaling_client_.Leave();
  }
}

// ============================================================================
//  观察者实现 —— DataChannelObserver（signaling 线程）
// ============================================================================

void webrtc_engine::OnStateChange() {
  if (!data_channel_) {
    return;
  }
  auto state = data_channel_->state();
  RTC_LOG(LS_INFO) << "DataChannel state: "
                   << DataChannelInterface::DataStateString(state);
  data_channel_open_ = (state == DataChannelInterface::kOpen);
  if (engine_observer_) {
    engine_observer_->OnDataChannelStateChange(state);
  }
}

void webrtc_engine::OnMessage(const DataBuffer& buffer) {
  last_received_message_ =
      std::string(buffer.data.data<char>(), buffer.data.size());
  RTC_LOG(LS_INFO) << "DataChannel onMessage: " << last_received_message_;
  if (engine_observer_) {
    engine_observer_->OnDataChannelMessage(last_received_message_);
  }
}

// ============================================================================
//  观察者实现 —— CreateSessionDescriptionObserver（CreateSdpObserver）
// ============================================================================

void webrtc_engine::CreateSdpObserver::OnSuccess(
    SessionDescriptionInterface* desc) {
  RTC_LOG(LS_INFO) << "Local SDP created (type=" << desc->GetType() << ")";
  std::string sdp;
  desc->ToString(&sdp);
  std::string type = engine_->is_offerer_ ? "offer" : "answer";
  if (engine_->engine_observer_) {
    engine_->engine_observer_->OnLocalSdp(type, sdp);
  }
  engine_->peer_connection_->SetLocalDescription(
      std::unique_ptr<SessionDescriptionInterface>(desc),
      make_ref_counted<SetLocalObserver>(engine_));
  if (engine_->is_offerer_) {
    if (engine_->renegotiating_) {
      engine_->signaling_client_.SendReoffer(engine_->peer_id_, sdp);
    } else {
      engine_->signaling_client_.Call(engine_->peer_id_, sdp);
    }
  } else {
    engine_->signaling_client_.Answer(engine_->peer_id_, sdp);
    // answerer 发出 answer 即 ICE 重启协商完成
    if (engine_->reconnect_in_progress_) {
      engine_->reconnect_in_progress_ = false;
      if (engine_->engine_observer_) engine_->engine_observer_->OnReconnected(true);
    }
  }
}

void webrtc_engine::CreateSdpObserver::OnFailure(RTCError error) {
  RTC_LOG(LS_ERROR) << "CreateOffer/Answer failed: " << error.message();
  if (engine_->engine_observer_) engine_->engine_observer_->OnEngineError("sdp_create_failed", error.message());
  if (engine_->reconnect_in_progress_) {
    engine_->reconnect_in_progress_ = false;
    if (engine_->engine_observer_) engine_->engine_observer_->OnReconnected(false);
  }
}

// ============================================================================
//  观察者实现 —— SetLocalDescriptionObserverInterface（SetLocalObserver）
// ============================================================================

void webrtc_engine::SetLocalObserver::OnSetLocalDescriptionComplete(
    RTCError error) {
  if (error.ok()) {
    RTC_LOG(LS_INFO) << "SetLocalDescription OK";
  } else {
    RTC_LOG(LS_ERROR) << "SetLocalDescription failed: " << error.message();
    if (engine_->engine_observer_) engine_->engine_observer_->OnEngineError("set_local_desc_failed", error.message());
  }
}

// ============================================================================
//  观察者实现 —— SetRemoteDescriptionObserverInterface（SetRemoteObserver）
// ============================================================================

void webrtc_engine::SetRemoteObserver::OnSetRemoteDescriptionComplete(
    RTCError error) {
  if (error.ok()) {
    RTC_LOG(LS_INFO) << "SetRemoteDescription OK";
    if (!engine_->is_offerer_) {
      // 答方：设完远端 offer 后生成 answer
      engine_->CreateAnswer();
    } else if (engine_->reconnect_in_progress_) {
      // offerer 收到答案（ICE 重启协商完成）
      engine_->reconnect_in_progress_ = false;
      if (engine_->engine_observer_) engine_->engine_observer_->OnReconnected(true);
    }
  } else {
    RTC_LOG(LS_ERROR) << "SetRemoteDescription failed: " << error.message();
    if (engine_->engine_observer_) engine_->engine_observer_->OnEngineError("set_remote_desc_failed", error.message());
    if (engine_->reconnect_in_progress_) {
      engine_->reconnect_in_progress_ = false;
      if (engine_->engine_observer_) engine_->engine_observer_->OnReconnected(false);
    }
  }
}

// ============================================================================
//  观察者实现 —— RTCStatsCollectorCallback（StatsCollector）
// ============================================================================

webrtc_engine::StatsCollector::StatsCollector(webrtc_engine* engine,
                                              EngineStats stats)
    : engine_(engine), stats_(std::move(stats)) {}

void webrtc_engine::StatsCollector::OnStatsDelivered(
    const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report) {
  if (report) {
    // 传输层总账（SCTP/DTLS/ICE 上的总流量）。
    for (const auto* t : report->GetStatsOfType<webrtc::RTCTransportStats>()) {
      if (t->bytes_sent) stats_.transport.bytes_sent = *t->bytes_sent;
      if (t->packets_sent) stats_.transport.packets_sent = *t->packets_sent;
      if (t->bytes_received)
        stats_.transport.bytes_received = *t->bytes_received;
      if (t->packets_received)
        stats_.transport.packets_received = *t->packets_received;
      if (t->dtls_state) stats_.transport.dtls_state = *t->dtls_state;
      if (t->ice_state) stats_.transport.ice_state = *t->ice_state;
      if (t->ice_role) stats_.transport.ice_role = *t->ice_role;
      if (t->selected_candidate_pair_id)
        stats_.transport.selected_candidate_pair_id =
            *t->selected_candidate_pair_id;
    }

    // 填充候选地址（local/remote 候选共用的字段）。
    auto fill_candidate = [](const auto* c, EngineStats::CandidateInfo* out) {
      if (c->ip) out->ip = *c->ip;
      else if (c->address) out->ip = *c->address;
      if (c->port) out->port = *c->port;
      if (c->protocol) out->protocol = *c->protocol;
      if (c->candidate_type) out->candidate_type = *c->candidate_type;
    };

    // 活跃候选对：优先「被提名且可写」，退而取第一个可写。
    const webrtc::RTCIceCandidatePairStats* active = nullptr;
    for (const auto* p :
         report->GetStatsOfType<webrtc::RTCIceCandidatePairStats>()) {
      if (!active && p->writable && *p->writable) {
        active = p;
      }
      if (p->nominated && *p->nominated && p->writable && *p->writable) {
        active = p;
        break;
      }
    }
    if (active) {
      if (active->state) stats_.candidate_pair.state = *active->state;
      if (active->nominated) stats_.candidate_pair.nominated = *active->nominated;
      if (active->writable) stats_.candidate_pair.writable = *active->writable;
      if (active->packets_sent)
        stats_.candidate_pair.packets_sent = *active->packets_sent;
      if (active->packets_received)
        stats_.candidate_pair.packets_received = *active->packets_received;
      if (active->bytes_sent)
        stats_.candidate_pair.bytes_sent = *active->bytes_sent;
      if (active->bytes_received)
        stats_.candidate_pair.bytes_received = *active->bytes_received;
      if (active->current_round_trip_time)
        stats_.candidate_pair.current_round_trip_time =
            *active->current_round_trip_time;
      if (active->total_round_trip_time)
        stats_.candidate_pair.total_round_trip_time =
            *active->total_round_trip_time;
      if (active->available_outgoing_bitrate)
        stats_.candidate_pair.available_outgoing_bitrate =
            *active->available_outgoing_bitrate;
      if (active->available_incoming_bitrate)
        stats_.candidate_pair.available_incoming_bitrate =
            *active->available_incoming_bitrate;
    }

    // 全部本端候选（selected 标记被选中的那一个，并回填 candidate_pair）。
    for (const auto* c :
         report->GetStatsOfType<webrtc::RTCLocalIceCandidateStats>()) {
      EngineStats::CandidateInfo info;
      fill_candidate(c, &info);
      info.selected = active && active->local_candidate_id &&
                      c->id() == *active->local_candidate_id;
      if (info.selected) stats_.candidate_pair.local_candidate = info;
      stats_.local_candidates.push_back(std::move(info));
    }
    // 全部对端候选（selected 标记被选中的那一个）。
    for (const auto* c :
         report->GetStatsOfType<webrtc::RTCRemoteIceCandidateStats>()) {
      EngineStats::CandidateInfo info;
      fill_candidate(c, &info);
      info.selected = active && active->remote_candidate_id &&
                      c->id() == *active->remote_candidate_id;
      if (info.selected) stats_.candidate_pair.remote_candidate = info;
      stats_.remote_candidates.push_back(std::move(info));
    }

    // PeerConnection 级计数。
    for (const auto* p :
         report->GetStatsOfType<webrtc::RTCPeerConnectionStats>()) {
      if (p->data_channels_opened)
        stats_.data_channels_opened = *p->data_channels_opened;
      if (p->data_channels_closed)
        stats_.data_channels_closed = *p->data_channels_closed;
    }

    // 编解码器名映射（codec_id -> mime_type，用于 RTP 流里解析 codec）。
    std::map<std::string, std::string> codec_names;
    for (const auto* c : report->GetStatsOfType<webrtc::RTCCodecStats>()) {
      if (c->mime_type) codec_names[c->id()] = *c->mime_type;
    }

    // 音视频：接收流（当前无 track 时为空，字段预留）。
    for (const auto* s :
         report->GetStatsOfType<webrtc::RTCInboundRtpStreamStats>()) {
      EngineStats::InboundRtpInfo info;
      if (s->kind) info.kind = *s->kind;
      if (s->ssrc) info.ssrc = *s->ssrc;
      if (s->codec_id) {
        auto it = codec_names.find(*s->codec_id);
        if (it != codec_names.end()) info.codec = it->second;
      }
      if (s->packets_received) info.packets_received = *s->packets_received;
      if (s->bytes_received) info.bytes_received = *s->bytes_received;
      if (s->packets_lost) info.packets_lost = *s->packets_lost;
      if (s->jitter) info.jitter = *s->jitter;
      if (s->frames_per_second) info.frames_per_second = *s->frames_per_second;
      if (s->frame_width) info.frame_width = *s->frame_width;
      if (s->frame_height) info.frame_height = *s->frame_height;
      if (s->frames_decoded) info.frames_decoded = *s->frames_decoded;
      if (s->frames_dropped) info.frames_dropped = *s->frames_dropped;
      if (s->audio_level) info.audio_level = *s->audio_level;
      stats_.inbound_rtp.push_back(std::move(info));
    }

    // 音视频：发送流（字段预留）。
    for (const auto* s :
         report->GetStatsOfType<webrtc::RTCOutboundRtpStreamStats>()) {
      EngineStats::OutboundRtpInfo info;
      if (s->kind) info.kind = *s->kind;
      if (s->ssrc) info.ssrc = *s->ssrc;
      if (s->codec_id) {
        auto it = codec_names.find(*s->codec_id);
        if (it != codec_names.end()) info.codec = it->second;
      }
      if (s->packets_sent) info.packets_sent = *s->packets_sent;
      if (s->bytes_sent) info.bytes_sent = *s->bytes_sent;
      if (s->target_bitrate) info.target_bitrate = *s->target_bitrate;
      if (s->frames_encoded) info.frames_encoded = *s->frames_encoded;
      if (s->frames_per_second) info.frames_per_second = *s->frames_per_second;
      if (s->frame_width) info.frame_width = *s->frame_width;
      if (s->frame_height) info.frame_height = *s->frame_height;
      if (s->quality_limitation_reason)
        info.quality_limitation_reason = *s->quality_limitation_reason;
      stats_.outbound_rtp.push_back(std::move(info));
    }

    // 音视频：对端反馈（我们发的流在对方那边的接收质量，字段预留）。
    for (const auto* s :
         report->GetStatsOfType<webrtc::RTCRemoteInboundRtpStreamStats>()) {
      EngineStats::RemoteInboundRtpInfo info;
      if (s->kind) info.kind = *s->kind;
      if (s->ssrc) info.ssrc = *s->ssrc;
      if (s->round_trip_time) info.round_trip_time = *s->round_trip_time;
      if (s->fraction_lost) info.fraction_lost = *s->fraction_lost;
      if (s->jitter) info.jitter = *s->jitter;
      stats_.remote_inbound_rtp.push_back(std::move(info));
    }
  }

  if (engine_ && engine_->engine_observer_) {
    engine_->engine_observer_->OnStats(stats_);
  }
}
