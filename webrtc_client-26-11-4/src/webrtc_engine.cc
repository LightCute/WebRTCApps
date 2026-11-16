#include "webrtc_engine.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <map>
#include <random>

#include "api/audio/audio_device.h"
#include "api/audio/create_audio_device_module.h"
#include "api/create_modular_peer_connection_factory.h"
#include "api/environment/environment.h"
#include "api/environment/environment_factory.h"
#include "api/field_trials.h"
#include "api/rtp_parameters.h"
#include "api/stats/rtcstats_objects.h"
#include "media/audio_mute.h"
#include "modules/audio_device/audio_device_impl.h"
#include "media/video_codec.h"
#include "media/video_device.h"
#include "media/video_device_monitor.h"
#include "media/video_source.h"
#include "rtc_base/logging.h"
#include "rtc_base/ssl_adapter.h"

using namespace webrtc;

namespace {

// "maintain_resolution" -> MAINTAIN_RESOLUTION，未知回 BALANCED。
DegradationPreference StringToDegradation(const std::string& mode) {
  if (mode == "maintain_resolution") return DegradationPreference::MAINTAIN_RESOLUTION;
  if (mode == "maintain_framerate") return DegradationPreference::MAINTAIN_FRAMERATE;
  if (mode == "disabled") return DegradationPreference::DISABLED;
  return DegradationPreference::BALANCED;
}

bool ParseOnOff(const std::string& value, bool* out) {
  if (value == "on" || value == "true" || value == "1") {
    *out = true;
    return true;
  }
  if (value == "off" || value == "false" || value == "0") {
    *out = false;
    return true;
  }
  return false;
}

// ADM 的 index 0 是 "default: <当前默认源>" 别名，名字会随默认设备变化，
// 不能参与热插拔的集合比较（否则会误报 removed/added）。
bool IsDefaultAudioAlias(const std::string& name) {
  return name.rfind("default:", 0) == 0;
}

}  // namespace

// ============================================================================
//  构造 / 析构
// ============================================================================

webrtc_engine::webrtc_engine() {
  rtc_configuration_.sdp_semantics = SdpSemantics::kUnifiedPlan;
  sdp_observer_ = make_ref_counted<CreateSdpObserver>(this);
  audio_processing_config_ = webrtc_media::DefaultAudioProcessingConfig();
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

  // 设备基线（热插拔对比用），必须在启动监听前建立，否则首次变化会被误判为“新增”。
  known_device_ids_.clear();
  for (const auto& dev : webrtc_media::EnumerateVideoDevices()) {
    known_device_ids_.push_back(dev.id);
  }

  // 视频设备热插拔监听（netlink uevent）。回调 marshal 到 signaling 线程。
  device_monitor_ = std::make_unique<webrtc_media::VideoDeviceMonitor>();
  if (!device_monitor_->Start(
          [this](const std::vector<webrtc_media::VideoDeviceInfo>& devices) {
            if (!signaling_thread_) {
              return;
            }
            signaling_thread_->PostTask(
                [this, devices] { OnVideoDevicesChangedImpl(devices); });
          })) {
    RTC_LOG(LS_WARNING) << "VideoDeviceMonitor start failed";
    device_monitor_.reset();
  }

  // 音频设备热插拔监听。基线在 ADM 首次创建时建立（可能晚于 Start）。
  audio_devices_baseline_initialized_ = false;
  known_audio_device_ids_.clear();
  audio_device_monitor_ = std::make_unique<webrtc_media::AudioDeviceMonitor>();
  if (!audio_device_monitor_->Start([this] {
        if (!signaling_thread_) {
          return;
        }
        signaling_thread_->PostTask([this] { HandleAudioDevicesChanged(); });
      })) {
    RTC_LOG(LS_WARNING) << "AudioDeviceMonitor start failed";
    audio_device_monitor_.reset();
  }
  return true;
}

void webrtc_engine::Stop() {
  // 先停周期性统计（避免线程销毁后还有延时任务回调）。
  video_stats_enabled_ = false;
  audio_stats_enabled_ = false;
  // 再停热插拔监听（避免拆卸过程中再回调）。
  if (device_monitor_) {
    device_monitor_->Stop();
    device_monitor_.reset();
  }
  if (audio_device_monitor_) {
    audio_device_monitor_->Stop();
    audio_device_monitor_.reset();
  }
  // 再清理 WebRTC 对象（PC/DC/factory 必须在 signaling 线程销毁），最后停线程。
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
  // 已有重协商在途（video_on/off、audio_on/off、reconnect、上一次 switch）：
  // 直接重建 PC 会让在途 offer/answer 落到已销毁的 PC 上。延迟到本轮完成。
  if (video_switch_in_progress_ || audio_switch_in_progress_ ||
      reconnect_in_progress_ || ice_switch_in_progress_) {
    pending_switch_mode_ = mode;
    RTC_LOG(LS_INFO) << "SwitchIce deferred until renegotiation completes: "
                     << mode;
    return;
  }
  SetIceTransports(mode);  // 只改本端 mode
  RTC_LOG(LS_INFO) << "SwitchIce mode=" << mode
                   << " type=" << static_cast<int>(rtc_configuration_.type)
                   << " servers=" << rtc_configuration_.servers.size();
  ice_switch_in_progress_ = true;
  signaling_client_.SendSwitchIce(peer_id_);
  RebuildAndOffer();  // 命令方重建 PC + offer（对称：谁被命令谁 offer）
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

void webrtc_engine::MaybeRunPendingSwitch() {
  if (!pending_switch_mode_.has_value()) {
    return;
  }
  if (video_switch_in_progress_ || audio_switch_in_progress_ ||
      reconnect_in_progress_ || ice_switch_in_progress_) {
    return;  // 还有协商在途
  }
  const std::string mode = *pending_switch_mode_;
  pending_switch_mode_.reset();
  RTC_LOG(LS_INFO) << "Running deferred SwitchIce: " << mode;
  // 可能在 PC 观察者回调里被调用（OnConnectionChange），延后一个任务再重建。
  if (signaling_thread_) {
    signaling_thread_->PostTask([this, mode] { SwitchIce(mode); });
  } else {
    SwitchIce(mode);
  }
}

void webrtc_engine::ReconnectImpl() {
  RTC_LOG(LS_INFO) << "Reconnect";
  reconnect_in_progress_ = true;
  signaling_client_.SendReconnect(peer_id_);  // 告诉对端「这是 reconnect」，对端标记完成检测
  CreateOfferIceRestart();  // 命令方直接 ICE 重启 + offer（对称）
}

// 视频开关（第一阶段：合成测试图源；空闲时开，通话中暂不支持）。
void webrtc_engine::StartVideo() {
  if (!signaling_thread_) {
    return;
  }
  if (signaling_thread_->IsCurrent()) {
    StartVideoImpl();
    return;
  }
  signaling_thread_->BlockingCall([&] { StartVideoImpl(); });
}

void webrtc_engine::StopVideo() {
  if (!signaling_thread_) {
    return;
  }
  if (signaling_thread_->IsCurrent()) {
    StopVideoImpl();
    return;
  }
  signaling_thread_->BlockingCall([&] { StopVideoImpl(); });
}

void webrtc_engine::StartVideoImpl() {
  video_enabled_ = true;
  RTC_LOG(LS_INFO) << "StartVideo";
  if (engine_observer_) {
    engine_observer_->OnVideoStateChanged(true);
  }
}

void webrtc_engine::StopVideoImpl() {
  video_enabled_ = false;
  RemoveVideoTrackFromPc();
  RTC_LOG(LS_INFO) << "StopVideo";
  if (engine_observer_) {
    engine_observer_->OnVideoStateChanged(false);
  }
}

// 通话中开视频轨（AddTrack + 重新 offer/answer）。
void webrtc_engine::VideoOn() {
  if (!signaling_thread_) {
    return;
  }
  if (signaling_thread_->IsCurrent()) {
    VideoOnImpl();
    return;
  }
  signaling_thread_->BlockingCall([&] { VideoOnImpl(); });
}

// 通话中关视频轨（RemoveTrack + 重新 offer/answer）。
void webrtc_engine::VideoOff() {
  if (!signaling_thread_) {
    return;
  }
  if (signaling_thread_->IsCurrent()) {
    VideoOffImpl();
    return;
  }
  signaling_thread_->BlockingCall([&] { VideoOffImpl(); });
}

void webrtc_engine::VideoOnImpl() {
  video_enabled_ = true;
  video_switch_in_progress_ = true;
  if (!AddVideoTrackToPc()) {
    video_switch_in_progress_ = false;
    RTC_LOG(LS_ERROR) << "VideoOn: AddVideoTrack failed";
    if (engine_observer_) engine_observer_->OnVideoSwitched(true, false);
    return;
  }
  TriggerRenegotiation();
  RTC_LOG(LS_INFO) << "VideoOn";
  if (engine_observer_) {
    engine_observer_->OnVideoStateChanged(true);
  }
}

void webrtc_engine::VideoOffImpl() {
  video_enabled_ = false;
  video_switch_in_progress_ = true;
  if (!RemoveVideoTrackFromPc()) {
    video_switch_in_progress_ = false;
    RTC_LOG(LS_ERROR) << "VideoOff: RemoveVideoTrack failed";
    if (engine_observer_) engine_observer_->OnVideoSwitched(false, false);
    return;
  }
  TriggerRenegotiation();
  RTC_LOG(LS_INFO) << "VideoOff";
  if (engine_observer_) {
    engine_observer_->OnVideoStateChanged(false);
  }
}

// 通用重协商触发：谁改轨谁 CreateOffer（对称重协商，双方都能 offer）。
void webrtc_engine::TriggerRenegotiation() {
  renegotiating_ = true;
  CreateOffer();
}

// 懒创建合成视频源 + VideoTrack（源可跨 PC 重建复用，track 每 PC 重建）。
void webrtc_engine::CreateVideoTrack() {
  if (video_track_) {
    return;
  }
  if (!video_source_) {
    if (!environment_) {
      RTC_LOG(LS_ERROR) << "CreateVideoTrack: environment not ready";
      return;
    }
    // 选设备：指定的 > 第一个真实设备 > 第一个合成设备。
    webrtc_media::VideoDeviceInfo dev;
    if (pending_device_id_.empty() ||
        !webrtc_media::FindVideoDevice(pending_device_id_, &dev)) {
      auto devices = webrtc_media::EnumerateVideoDevices();
      if (devices.empty()) {
        RTC_LOG(LS_ERROR) << "CreateVideoTrack: no video device available";
        return;
      }
      dev = devices.front();
    }
    // 采集档位：用户显式设置过就沿用，否则跟随设备默认值。
    if (!capture_profile_explicit_) {
      capture_width_ = dev.width;
      capture_height_ = dev.height;
      capture_fps_ = dev.fps;
    } else {
      dev.width = capture_width_;
      dev.height = capture_height_;
      dev.fps = capture_fps_;
    }
    video_source_ =
        webrtc_media::CreateVideoSource(dev, environment_->task_queue_factory());
    if (!video_source_) {
      RTC_LOG(LS_ERROR) << "CreateVideoTrack: failed to create source for "
                        << dev.id;
      return;
    }
    current_device_id_ = dev.id;
    pending_device_id_ = dev.id;
    RTC_LOG(LS_INFO) << "Video source created with device: " << dev.id;
  }
  if (!peer_connection_factory_) {
    RTC_LOG(LS_WARNING) << "CreateVideoTrack: factory not created";
    return;
  }
  video_track_ = peer_connection_factory_->CreateVideoTrack(video_source_, "video");
  RTC_LOG(LS_INFO) << "VideoTrack created";
}

bool webrtc_engine::AddVideoTrackToPc() {
  CreateVideoTrack();
  if (!peer_connection_ || !video_track_) {
    return false;
  }
  auto result = peer_connection_->AddTrack(video_track_, {"stream"});
  if (result.ok()) {
    video_sender_ = result.value();
    RTC_LOG(LS_INFO) << "Video track added to PC";
    return true;
  }
  RTC_LOG(LS_ERROR) << "AddTrack failed: " << result.error().message();
  return false;
}

bool webrtc_engine::RemoveVideoTrackFromPc() {
  if (peer_connection_ && video_sender_) {
    RTCError err = peer_connection_->RemoveTrackOrError(video_sender_);
    if (!err.ok()) {
      RTC_LOG(LS_ERROR) << "RemoveTrack failed: " << err.message();
      return false;
    }
  }
  video_sender_ = nullptr;
  video_track_ = nullptr;
  return true;
}

// ============================================================================
//  音频（第一期：采集上行 + 设备选择 + 音频处理开关）
// ============================================================================

void webrtc_engine::GetAudioDevices() {
  if (!signaling_thread_) return;
  if (signaling_thread_->IsCurrent()) { GetAudioDevicesImpl(); return; }
  signaling_thread_->BlockingCall([&] { GetAudioDevicesImpl(); });
}

bool webrtc_engine::SetAudioDevice(const std::string& name) {
  if (!signaling_thread_) return false;
  if (signaling_thread_->IsCurrent()) return SetAudioDeviceImpl(name);
  bool ok = false;
  signaling_thread_->BlockingCall([&] { ok = SetAudioDeviceImpl(name); });
  return ok;
}

void webrtc_engine::AudioOn() {
  if (!signaling_thread_) return;
  if (signaling_thread_->IsCurrent()) { AudioOnImpl(); return; }
  signaling_thread_->BlockingCall([&] { AudioOnImpl(); });
}

void webrtc_engine::AudioOff() {
  if (!signaling_thread_) return;
  if (signaling_thread_->IsCurrent()) { AudioOffImpl(); return; }
  signaling_thread_->BlockingCall([&] { AudioOffImpl(); });
}

void webrtc_engine::GetAudioStatus() {
  if (!signaling_thread_) return;
  if (signaling_thread_->IsCurrent()) { GetAudioStatusImpl(); return; }
  signaling_thread_->BlockingCall([&] { GetAudioStatusImpl(); });
}

bool webrtc_engine::SetAudioProcessing(const std::string& flag, bool on) {
  if (!signaling_thread_) return false;
  if (signaling_thread_->IsCurrent()) return SetAudioProcessingImpl(flag, on);
  bool ok = false;
  signaling_thread_->BlockingCall([&] { ok = SetAudioProcessingImpl(flag, on); });
  return ok;
}

void webrtc_engine::GetAudioConfig() {
  if (!signaling_thread_) return;
  if (signaling_thread_->IsCurrent()) { GetAudioConfigImpl(); return; }
  signaling_thread_->BlockingCall([&] { GetAudioConfigImpl(); });
}

bool webrtc_engine::SetAudioConfig(const std::string& key,
                                   const std::string& value,
                                   std::string* error) {
  if (!signaling_thread_) return false;
  if (signaling_thread_->IsCurrent()) {
    return SetAudioConfigImpl(key, value, error);
  }
  bool ok = false;
  signaling_thread_->BlockingCall(
      [&] { ok = SetAudioConfigImpl(key, value, error); });
  return ok;
}

void webrtc_engine::SetAudioStats(bool enabled, int interval_ms) {
  if (!signaling_thread_) return;
  if (signaling_thread_->IsCurrent()) {
    SetAudioStatsImpl(enabled, interval_ms);
    return;
  }
  signaling_thread_->BlockingCall(
      [&] { SetAudioStatsImpl(enabled, interval_ms); });
}

bool webrtc_engine::EnsureAudioReady() {
  if (adm_) {
    return true;
  }
  return InitializePeerConnectionFactoryImpl();
}

void webrtc_engine::HandleAudioDevicesChanged() {
  if (!adm_) {
    return;  // ADM 尚未创建：首次建 factory 时会建立基线
  }
  const auto devices = worker_thread_->BlockingCall([this] {
    return webrtc_media::EnumerateRecordingDevices(adm_.get());
  });
  std::set<std::string> ids;
  for (const auto& device : devices) {
    if (!IsDefaultAudioAlias(device.name)) {
      ids.insert(device.id.empty() ? device.name : device.id);
    }
  }
  if (!audio_devices_baseline_initialized_) {
    known_audio_device_ids_ = ids;
    audio_devices_baseline_initialized_ = true;
    return;
  }
  bool added = false;
  bool removed = false;
  for (const auto& id : ids) {
    if (known_audio_device_ids_.find(id) == known_audio_device_ids_.end()) {
      added = true;
    }
  }
  for (const auto& id : known_audio_device_ids_) {
    if (ids.find(id) == ids.end()) {
      removed = true;
    }
  }
  if (!added && !removed) {
    return;
  }
  known_audio_device_ids_ = ids;
  const std::string reason = removed ? "device_removed" : "device_added";
  RTC_LOG(LS_INFO) << "Audio devices changed: reason=" << reason
                   << " count=" << devices.size();
  if (engine_observer_) {
    engine_observer_->OnAudioDevicesChanged(devices, audio_device_id_, reason);
  }

  // 策略 A：当前选中的设备被拔出 → 关音频轨（与视频一致），插回只通知。
  const bool current_lost =
      !audio_device_id_.empty() && ids.find(audio_device_id_) == ids.end();
  if (!current_lost) {
    return;
  }
  const std::string lost = audio_device_id_;
  audio_device_id_.clear();
  audio_device_name_.clear();
  if (engine_observer_) {
    engine_observer_->OnAudioDeviceLost(lost);
  }
  if (audio_enabled_ && peer_connection_ && audio_sender_) {
    AudioOffImpl();
  }
}

void webrtc_engine::GetAudioDevicesImpl() {
  if (!EnsureAudioReady()) {
    RTC_LOG(LS_ERROR) << "GetAudioDevices: audio not ready";
  }
  std::vector<webrtc_media::AudioDeviceInfo> devices;
  if (adm_) {
    // Pulse ADM 有线程约束，枚举放 worker 线程。
    devices = worker_thread_->BlockingCall([this] {
      return webrtc_media::EnumerateRecordingDevices(adm_.get());
    });
  }
  // 未显式选择时回显空串 = 使用系统默认设备（ADM 不暴露当前索引的 getter）。
  const std::string current = audio_device_id_;
  if (engine_observer_) {
    engine_observer_->OnAudioDevices(devices, current);
  }
}

bool webrtc_engine::SetAudioDeviceImpl(const std::string& name) {
  if (!EnsureAudioReady() || !adm_) return false;
  const auto devices = worker_thread_->BlockingCall([this] {
    return webrtc_media::EnumerateRecordingDevices(adm_.get());
  });
  int target_index = -1;
  int old_index = -1;
  std::string target_id;
  std::string target_name;
  for (size_t i = 0; i < devices.size(); ++i) {
    const auto& device = devices[i];
    // id（Pulse source name）优先，兼容按描述名选择。
    if (device.id == name || device.name == name) {
      target_index = static_cast<int>(i);
      target_id = device.id;
      target_name = device.name;
    }
    if ((!audio_device_id_.empty() && device.id == audio_device_id_) ||
        (audio_device_id_.empty() && !audio_device_name_.empty() &&
         device.name == audio_device_name_)) {
      old_index = static_cast<int>(i);
    }
  }
  if (target_index < 0) {
    RTC_LOG(LS_WARNING) << "SetAudioDevice: not found: " << name;
    return false;
  }

  // 空闲 / 尚未开始录音：只记住，下次开音频生效。
  const bool should_switch_now =
      (peer_connection_ != nullptr) && audio_enabled_ && audio_sender_;
  if (!should_switch_now) {
    audio_device_id_ = target_id;
    audio_device_name_ = target_name;
    RTC_LOG(LS_INFO) << "SetAudioDevice (pending): " << target_id;
    if (engine_observer_) {
      engine_observer_->OnAudioDeviceChanged(target_id, target_name, true);
    }
    return true;
  }

  // 通话中：worker 线程执行 Stop → SetRecordingDevice → Init/Start。
  // 失败回滚到原设备并恢复录音状态。
  const int rollback_index = old_index >= 0 ? old_index : 0;
  const bool ok = worker_thread_->BlockingCall([this, target_index,
                                                rollback_index]() {
    const bool was_recording = adm_->Recording();
    auto restore = [&]() {
      adm_->StopRecording();
      if (adm_->SetRecordingDevice(
              static_cast<uint16_t>(rollback_index)) == 0 &&
          was_recording) {
        if (adm_->InitRecording() == 0) {
          adm_->StartRecording();
        }
      }
    };
    if (was_recording && adm_->StopRecording() != 0) {
      RTC_LOG(LS_WARNING) << "SetAudioDevice: StopRecording failed";
      return false;
    }
    if (adm_->SetRecordingDevice(static_cast<uint16_t>(target_index)) != 0) {
      RTC_LOG(LS_WARNING) << "SetAudioDevice: SetRecordingDevice failed";
      restore();
      return false;
    }
    if (was_recording) {
      if (adm_->InitRecording() != 0) {
        RTC_LOG(LS_WARNING) << "SetAudioDevice: InitRecording failed";
        restore();
        return false;
      }
      if (adm_->StartRecording() != 0) {
        RTC_LOG(LS_WARNING) << "SetAudioDevice: StartRecording failed";
        restore();
        return false;
      }
    }
    return true;
  });
  if (!ok) {
    RTC_LOG(LS_ERROR) << "SetAudioDevice (in call) failed: " << name;
    if (engine_observer_) {
      engine_observer_->OnAudioDeviceChanged(target_id, target_name, false);
    }
    return false;
  }

  audio_device_id_ = target_id;
  audio_device_name_ = target_name;
  RTC_LOG(LS_INFO) << "SetAudioDevice (in call): " << target_id;
  if (engine_observer_) {
    engine_observer_->OnAudioDeviceChanged(target_id, target_name, true);
  }
  return true;
}

// 音频开关：空闲时只记标志（下次 offer/answer 随轨道带上），通话中走重协商。
void webrtc_engine::AudioOnImpl() {
  if (audio_enabled_) {
    // 幂等：重复开启只回显，不重复加轨/重协商。
    if (engine_observer_) {
      engine_observer_->OnAudioStateChanged(true);
      engine_observer_->OnAudioSwitched(true, true);
    }
    return;
  }
  audio_enabled_ = true;
  if (!peer_connection_) {
    RTC_LOG(LS_INFO) << "AudioOn (pending until next call)";
    if (engine_observer_) {
      engine_observer_->OnAudioStateChanged(true);
      engine_observer_->OnAudioSwitched(true, true);
    }
    return;
  }
  audio_switch_in_progress_ = true;
  if (!AddAudioTrackToPc()) {
    audio_switch_in_progress_ = false;
    audio_enabled_ = false;
    RTC_LOG(LS_ERROR) << "AudioOn: AddAudioTrack failed";
    if (engine_observer_) engine_observer_->OnAudioSwitched(true, false);
    return;
  }
  TriggerRenegotiation();
  RTC_LOG(LS_INFO) << "AudioOn";
  if (engine_observer_) engine_observer_->OnAudioStateChanged(true);
}

void webrtc_engine::AudioOffImpl() {
  if (!audio_enabled_) {
    if (engine_observer_) {
      engine_observer_->OnAudioStateChanged(false);
      engine_observer_->OnAudioSwitched(false, true);
    }
    return;
  }
  audio_enabled_ = false;
  if (!peer_connection_ || !audio_sender_) {
    RTC_LOG(LS_INFO) << "AudioOff (pending / no track)";
    if (engine_observer_) {
      engine_observer_->OnAudioStateChanged(false);
      engine_observer_->OnAudioSwitched(false, true);
    }
    return;
  }
  audio_switch_in_progress_ = true;
  if (!RemoveAudioTrackFromPc()) {
    audio_switch_in_progress_ = false;
    RTC_LOG(LS_ERROR) << "AudioOff: RemoveAudioTrack failed";
    if (engine_observer_) engine_observer_->OnAudioSwitched(false, false);
    return;
  }
  TriggerRenegotiation();
  RTC_LOG(LS_INFO) << "AudioOff";
  if (engine_observer_) engine_observer_->OnAudioStateChanged(false);
}

void webrtc_engine::GetAudioStatusImpl() {
  if (!engine_observer_) return;
  EnsureAudioReady();  // 首次查询时也让 has_device_module 准确
  AudioStateSnapshot state;
  state.enabled = audio_enabled_;
  state.device_id = audio_device_id_;
  state.device_name = audio_device_name_;
  state.in_call = (peer_connection_ != nullptr);
  const webrtc_media::AudioProcessingState flags =
      webrtc_media::StateFromConfig(audio_processing_config_);
  state.aec = flags.aec;
  state.ns = flags.ns;
  state.agc = flags.agc;
  state.has_device_module = (adm_ != nullptr);
  engine_observer_->OnAudioStatus(state);
}

AudioConfigSnapshot webrtc_engine::BuildAudioConfigSnapshot() const {
  AudioConfigSnapshot snapshot;
  snapshot.processing = audio_processing_config_;
  snapshot.source = audio_source_options_;
  snapshot.pending_source = pending_source_options_;
  snapshot.device_id = audio_device_id_;
  snapshot.device_name = audio_device_name_;
  snapshot.in_call = (peer_connection_ != nullptr);
  snapshot.has_device_module = (adm_ != nullptr);
  snapshot.mic_muted = mic_muted_;
  snapshot.speaker_volume_available = speaker_volume_available_;
  snapshot.speaker_volume = speaker_volume_;
  snapshot.speaker_mute_available = speaker_mute_available_;
  snapshot.speaker_muted = speaker_muted_;
  snapshot.recording_sample_rate = recording_sample_rate_;
  snapshot.playout_sample_rate = playout_sample_rate_;
  snapshot.stereo_recording = stereo_recording_;
  snapshot.stereo_playout = stereo_playout_;
  return snapshot;
}

void webrtc_engine::GetAudioConfigImpl() {
  EnsureAudioReady();  // 首次查询时也让 device 字段准确
  RefreshSpeakerState();
  RefreshDeviceState();
  if (engine_observer_) {
    engine_observer_->OnAudioConfig(BuildAudioConfigSnapshot());
  }
}

bool webrtc_engine::SetAudioConfigImpl(const std::string& key,
                                       const std::string& value,
                                       std::string* error) {
  // 软件静音：只改本进程的采集帧（不清 Pulse source）。
  if (key == "mic_muted") {
    bool muted = false;
    if (!ParseOnOff(value, &muted)) {
      if (error != nullptr) *error = "invalid value for mic_muted: " + value;
      return false;
    }
    mic_muted_ = muted;
    if (mic_muted_flag_) {
      mic_muted_flag_->store(muted, std::memory_order_relaxed);
    }
    if (engine_observer_) {
      engine_observer_->OnAudioConfig(BuildAudioConfigSnapshot());
    }
    return true;
  }

  // 扬声器音量（0..100，映射到 ADM 的 min/max）。
  if (key == "speaker_volume") {
    errno = 0;
    char* end = nullptr;
    const long percent = std::strtol(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != '\0' || percent < 0 ||
        percent > 100) {
      if (error != nullptr) {
        *error = "invalid value for speaker_volume: " + value;
      }
      return false;
    }
    SetSpeakerVolumePercent(static_cast<int>(percent));
    if (engine_observer_) {
      engine_observer_->OnAudioConfig(BuildAudioConfigSnapshot());
    }
    return true;
  }

  if (key == "speaker_muted") {
    bool muted = false;
    if (!ParseOnOff(value, &muted)) {
      if (error != nullptr) *error = "invalid value for speaker_muted: " + value;
      return false;
    }
    SetSpeakerMuted(muted);
    if (engine_observer_) {
      engine_observer_->OnAudioConfig(BuildAudioConfigSnapshot());
    }
    return true;
  }

  // 源参数（AudioOptions）：只存 pending，下次建音轨时生效。
  if (key.rfind("source.", 0) == 0) {
    std::string err;
    if (!webrtc_media::SetAudioSourceOption(&audio_source_options_, key, value,
                                            &err)) {
      if (error != nullptr) {
        *error = err;
      }
      return false;
    }
    if (std::find(pending_source_options_.begin(),
                  pending_source_options_.end(),
                  key) == pending_source_options_.end()) {
      pending_source_options_.push_back(key);
    }
    if (engine_observer_) {
      engine_observer_->OnAudioConfig(BuildAudioConfigSnapshot());
    }
    return true;
  }

  std::string err;
  if (!webrtc_media::SetAudioProcessingKey(&audio_processing_config_, key, value,
                                           &err)) {
    if (error != nullptr) {
      *error = err;
    }
    return false;
  }
  ReapplyAudioProcessing();
  if (engine_observer_) {
    engine_observer_->OnAudioConfig(BuildAudioConfigSnapshot());
  }
  return true;
}

void webrtc_engine::SetAudioStatsImpl(bool enabled, int interval_ms) {
  const bool was_enabled = audio_stats_enabled_;
  audio_stats_enabled_ = enabled;
  if (!enabled) {
    return;
  }
  audio_stats_interval_ms_ =
      interval_ms <= 0 ? 2000 : std::clamp(interval_ms, 500, 10000);
  if (!was_enabled) {
    CollectAudioStats();
  }
}

void webrtc_engine::CollectAudioStats() {
  if (!audio_stats_enabled_) {
    return;  // 已关闭，停止自续期
  }
  // 续期放在发起侧：PC 重建时旧 PC 的 GetStats 回调可能丢失。
  ScheduleAudioStats();
  if (!peer_connection_) {
    return;
  }
  peer_connection_->GetStats(make_ref_counted<AudioStatsCollector>(this).get());
}

void webrtc_engine::ScheduleAudioStats() {
  if (!audio_stats_enabled_ || !signaling_thread_) {
    return;
  }
  signaling_thread_->PostDelayedTask(
      [this] { CollectAudioStats(); },
      TimeDelta::Millis(audio_stats_interval_ms_));
}

void webrtc_engine::AudioStatsCollector::OnStatsDelivered(
    const scoped_refptr<const RTCStatsReport>& report) {
  if (engine_ == nullptr) {
    return;
  }
  AudioStats stats;
  stats.enabled = engine_->audio_stats_enabled_;
  stats.in_call = engine_->peer_connection_ != nullptr;
  if (report) {
    std::map<std::string, std::string> codec_names;
    for (const auto* c : report->GetStatsOfType<RTCCodecStats>()) {
      if (c->mime_type) codec_names[c->id()] = *c->mime_type;
    }
    for (const auto* s : report->GetStatsOfType<RTCInboundRtpStreamStats>()) {
      if (!s->kind || *s->kind != "audio") {
        continue;
      }
      if (s->codec_id) {
        auto it = codec_names.find(*s->codec_id);
        if (it != codec_names.end()) stats.recv_codec = it->second;
      }
      if (s->packets_received) stats.packets_received = *s->packets_received;
      if (s->bytes_received) stats.bytes_received = *s->bytes_received;
      if (s->packets_lost) stats.packets_lost = *s->packets_lost;
      if (s->jitter) stats.jitter_ms = *s->jitter * 1000.0;
      if (s->audio_level) stats.audio_level = *s->audio_level;
      break;  // 音频只有一路
    }
    for (const auto* s : report->GetStatsOfType<RTCOutboundRtpStreamStats>()) {
      if (!s->kind || *s->kind != "audio") {
        continue;
      }
      if (s->codec_id) {
        auto it = codec_names.find(*s->codec_id);
        if (it != codec_names.end()) stats.send_codec = it->second;
      }
      if (s->packets_sent) stats.packets_sent = *s->packets_sent;
      if (s->bytes_sent) stats.bytes_sent = *s->bytes_sent;
      if (s->target_bitrate) stats.target_bitrate = *s->target_bitrate;
      break;
    }
    for (const auto* s :
         report->GetStatsOfType<RTCRemoteInboundRtpStreamStats>()) {
      if (!s->kind || *s->kind != "audio") {
        continue;
      }
      if (s->round_trip_time) stats.rtt_ms = *s->round_trip_time * 1000.0;
      if (s->fraction_lost) stats.fraction_lost = *s->fraction_lost;
      break;
    }
  }
  if (engine_->apm_) {
    const AudioProcessingStats apm_stats = engine_->apm_->GetStatistics(true);
    if (apm_stats.residual_echo_likelihood) {
      stats.residual_echo_likelihood = *apm_stats.residual_echo_likelihood;
    }
    if (apm_stats.echo_return_loss) {
      stats.echo_return_loss = *apm_stats.echo_return_loss;
    }
    if (apm_stats.delay_median_ms) {
      stats.delay_median_ms = static_cast<double>(*apm_stats.delay_median_ms);
    }
  }
  if (engine_->engine_observer_) {
    engine_->engine_observer_->OnAudioStats(stats);
  }
}

bool webrtc_engine::SetAudioProcessingImpl(const std::string& flag, bool on) {
  std::string key;
  if (flag == "aec") {
    key = "echo_canceller.enabled";
  } else if (flag == "ns") {
    key = "noise_suppression.enabled";
  } else if (flag == "agc") {
    key = "gain_controller1.enabled";
  } else {
    RTC_LOG(LS_WARNING) << "SetAudioProcessing: unknown flag " << flag;
    return false;
  }
  std::string err;
  if (!webrtc_media::SetAudioProcessingKey(&audio_processing_config_, key,
                                           on ? "on" : "off", &err)) {
    RTC_LOG(LS_WARNING) << "SetAudioProcessing: " << err;
    return false;
  }
  ReapplyAudioProcessing();
  if (engine_observer_) {
    engine_observer_->OnAudioProcessingChanged(
        webrtc_media::StateFromConfig(audio_processing_config_));
    engine_observer_->OnAudioConfig(BuildAudioConfigSnapshot());
  }
  return true;
}

void webrtc_engine::CreateAudioTrack() {
  if (audio_track_) return;
  if (!peer_connection_factory_) {
    RTC_LOG(LS_WARNING) << "CreateAudioTrack: factory not created";
    return;
  }
  const webrtc::AudioProcessing::Config& cfg = audio_processing_config_;
  AudioOptions options;
  options.echo_cancellation = cfg.echo_canceller.enabled;
  options.noise_suppression = cfg.noise_suppression.enabled;
  options.auto_gain_control = cfg.gain_controller1.enabled;
  options.highpass_filter = cfg.high_pass_filter.enabled;
  options.stereo_swapping = audio_source_options_.stereo_swapping;
  options.audio_jitter_buffer_max_packets =
      audio_source_options_.jitter_max_packets;
  options.audio_jitter_buffer_fast_accelerate =
      audio_source_options_.jitter_fast_accelerate;
  options.audio_jitter_buffer_min_delay_ms =
      audio_source_options_.jitter_min_delay_ms;
  // 只采集也要开始录音（WebRTC 默认 true，这里显式声明）。
  options.init_recording_on_send = audio_source_options_.init_recording_on_send;
  audio_source_ = peer_connection_factory_->CreateAudioSource(options);
  if (!audio_source_) {
    RTC_LOG(LS_ERROR) << "CreateAudioTrack: CreateAudioSource failed";
    return;
  }
  // 源参数已随本次建轨生效。
  pending_source_options_.clear();
  audio_track_ =
      peer_connection_factory_->CreateAudioTrack("audio", audio_source_.get());
  RTC_LOG(LS_INFO) << "AudioTrack created";
}

bool webrtc_engine::AddAudioTrackToPc() {
  CreateAudioTrack();
  if (!peer_connection_ || !audio_track_) return false;
  ApplySelectedAudioDevice();  // 必须在 InitRecording 之前
  auto result = peer_connection_->AddTrack(audio_track_, {"stream"});
  if (result.ok()) {
    audio_sender_ = result.value();
    // 音轨创建会触发 WebRtcVoiceSendChannel::SetOptions，可能覆盖 APM 配置，
    // 这里按当前开关重新应用一次。
    ReapplyAudioProcessing();
    RTC_LOG(LS_INFO) << "Audio track added to PC";
    return true;
  }
  RTC_LOG(LS_ERROR) << "AddTrack(audio) failed: " << result.error().message();
  return false;
}

bool webrtc_engine::RemoveAudioTrackFromPc() {
  if (peer_connection_ && audio_sender_) {
    RTCError err = peer_connection_->RemoveTrackOrError(audio_sender_);
    if (!err.ok()) {
      RTC_LOG(LS_ERROR) << "RemoveTrack(audio) failed: " << err.message();
      return false;
    }
  }
  audio_sender_ = nullptr;
  audio_track_ = nullptr;
  return true;
}

void webrtc_engine::ApplySelectedAudioDevice() {
  if (!adm_ || audio_device_name_.empty()) {
    return;  // 用系统默认设备
  }
  const std::string name = audio_device_name_;
  worker_thread_->BlockingCall([this, name] {
    webrtc_media::SetRecordingDeviceByName(adm_.get(), name);
  });
}

void webrtc_engine::ReapplyAudioProcessing() {
  if (!apm_) return;
  const webrtc::AudioProcessing::Config config = audio_processing_config_;
  worker_thread_->BlockingCall([this, config] {
    webrtc_media::ApplyAudioProcessingConfig(apm_.get(), config);
  });
}

void webrtc_engine::RefreshSpeakerState() {
  if (!adm_) return;
  worker_thread_->BlockingCall([this] {
    bool volume_available = false;
    speaker_volume_available_ =
        adm_->SpeakerVolumeIsAvailable(&volume_available) == 0 &&
        volume_available;
    if (speaker_volume_available_) {
      uint32_t max_volume = 0;
      uint32_t min_volume = 0;
      uint32_t volume = 0;
      if (adm_->MaxSpeakerVolume(&max_volume) == 0 &&
          adm_->MinSpeakerVolume(&min_volume) == 0 &&
          adm_->SpeakerVolume(&volume) == 0 && max_volume > min_volume) {
        speaker_volume_ = static_cast<int>(
            (volume - min_volume) * 100 / (max_volume - min_volume));
      }
    }
    bool mute_available = false;
    speaker_mute_available_ =
        adm_->SpeakerMuteIsAvailable(&mute_available) == 0 && mute_available;
    if (speaker_mute_available_) {
      bool muted = false;
      if (adm_->SpeakerMute(&muted) == 0) {
        speaker_muted_ = muted;
      }
    }
  });
}

void webrtc_engine::RefreshDeviceState() {
  if (!adm_) return;
  worker_thread_->BlockingCall([this] {
    auto* impl = static_cast<webrtc::AudioDeviceModuleImpl*>(adm_.get());
    if (impl != nullptr) {
      webrtc::AudioDeviceBuffer* buffer = impl->GetAudioDeviceBuffer();
      if (buffer != nullptr) {
        recording_sample_rate_ =
            static_cast<int>(buffer->RecordingSampleRate());
        playout_sample_rate_ =
            static_cast<int>(buffer->PlayoutSampleRate());
      }
    }
    bool stereo = false;
    if (adm_->StereoRecording(&stereo) == 0) {
      stereo_recording_ = stereo;
    }
    if (adm_->StereoPlayout(&stereo) == 0) {
      stereo_playout_ = stereo;
    }
  });
}

void webrtc_engine::SetSpeakerVolumePercent(int percent) {
  if (!adm_) return;
  worker_thread_->BlockingCall([this, percent] {
    bool volume_available = false;
    speaker_volume_available_ =
        adm_->SpeakerVolumeIsAvailable(&volume_available) == 0 &&
        volume_available;
    if (!speaker_volume_available_) {
      return;
    }
    uint32_t max_volume = 0;
    uint32_t min_volume = 0;
    if (adm_->MaxSpeakerVolume(&max_volume) != 0 ||
        adm_->MinSpeakerVolume(&min_volume) != 0 ||
        max_volume <= min_volume) {
      return;
    }
    const uint32_t volume =
        min_volume + (max_volume - min_volume) * percent / 100;
    if (adm_->SetSpeakerVolume(volume) == 0) {
      speaker_volume_ = percent;
    }
  });
}

void webrtc_engine::SetSpeakerMuted(bool muted) {
  if (!adm_) return;
  worker_thread_->BlockingCall([this, muted] {
    bool mute_available = false;
    speaker_mute_available_ =
        adm_->SpeakerMuteIsAvailable(&mute_available) == 0 && mute_available;
    if (!speaker_mute_available_) {
      return;
    }
    if (adm_->SetSpeakerMute(muted) == 0) {
      speaker_muted_ = muted;
    }
  });
}

// ============================================================================
//  视频参数控制（SetParameters 动态改，不重协商）
// ============================================================================

void webrtc_engine::SetBitrate(int max_kbps) {
  if (!signaling_thread_) return;
  if (signaling_thread_->IsCurrent()) { SetBitrateImpl(max_kbps); return; }
  signaling_thread_->BlockingCall([&] { SetBitrateImpl(max_kbps); });
}

void webrtc_engine::SetFramerate(int fps) {
  if (!signaling_thread_) return;
  if (signaling_thread_->IsCurrent()) { SetFramerateImpl(fps); return; }
  signaling_thread_->BlockingCall([&] { SetFramerateImpl(fps); });
}

void webrtc_engine::SetDegradation(const std::string& mode) {
  if (!signaling_thread_) return;
  if (signaling_thread_->IsCurrent()) { SetDegradationImpl(mode); return; }
  signaling_thread_->BlockingCall([&] { SetDegradationImpl(mode); });
}

void webrtc_engine::GetVideoStatus() {
  if (!signaling_thread_) return;
  if (signaling_thread_->IsCurrent()) { GetVideoStatusImpl(); return; }
  signaling_thread_->BlockingCall([&] { GetVideoStatusImpl(); });
}

// ============================================================================
//  视频设备：枚举 / 选择 / 通话中切换
// ============================================================================

void webrtc_engine::GetVideoDevices() {
  if (!signaling_thread_) return;
  if (signaling_thread_->IsCurrent()) { GetVideoDevicesImpl(); return; }
  signaling_thread_->BlockingCall([&] { GetVideoDevicesImpl(); });
}

bool webrtc_engine::SetVideoDevice(const std::string& id) {
  if (!signaling_thread_) return false;
  if (signaling_thread_->IsCurrent()) return SetVideoDeviceImpl(id);
  bool ok = false;
  signaling_thread_->BlockingCall([&] { ok = SetVideoDeviceImpl(id); });
  return ok;
}

void webrtc_engine::GetVideoDevicesImpl() {
  auto devices = webrtc_media::EnumerateVideoDevices();
  // 还没建源时，回显「默认会选的设备」（第一个真实设备，没有则合成设备）。
  std::string current = current_device_id_;
  if (current.empty() && !devices.empty()) {
    current = devices.front().id;
  }
  if (engine_observer_) {
    engine_observer_->OnVideoDevices(devices, current);
  }
}

bool webrtc_engine::SetVideoDeviceImpl(const std::string& id) {
  webrtc_media::VideoDeviceInfo dev;
  if (!webrtc_media::FindVideoDevice(id, &dev)) {
    RTC_LOG(LS_WARNING) << "SetVideoDevice: not found: " << id;
    return false;
  }
  if (!video_source_) {
    // 空闲：记住，下次建源生效。
    pending_device_id_ = id;
    current_device_id_ = id;
    capture_width_ = dev.width;
    capture_height_ = dev.height;
    capture_fps_ = dev.fps;
    capture_profile_explicit_ = false;
    RTC_LOG(LS_INFO) << "SetVideoDevice (idle): " << id;
    if (engine_observer_) {
      engine_observer_->OnVideoDeviceChanged(id, dev.name, true);
    }
    GetVideoStatusImpl();  // 回显当前设备/档位
    return true;
  }
  // 通话中（或已建源）：在本线程切换 capturer（不重协商）。
  // 注意：必须在**创建 capturer 的同一线程**销毁旧 capturer ——
  // VideoCaptureModuleV4L2 的 StopCapture() 有 RTC_DCHECK_RUN_ON(api_checker_)，
  // 换线程会直接 abort。capturer 都在 signaling 线程创建，故此处不能用
  // worker_thread_->BlockingCall。
  bool ok = webrtc_media::SwitchVideoSourceDevice(video_source_.get(), dev);
  if (ok) {
    pending_device_id_ = id;
    current_device_id_ = id;
    // 换设备后采集档位跟随新设备默认值。
    capture_width_ = dev.width;
    capture_height_ = dev.height;
    capture_fps_ = dev.fps;
    capture_profile_explicit_ = false;
  }
  RTC_LOG(LS_INFO) << "SetVideoDevice: " << id << " ok=" << ok;
  if (engine_observer_) {
    engine_observer_->OnVideoDeviceChanged(
        ok ? id : current_device_id_, dev.name, ok);
  }
  GetVideoStatusImpl();  // 回显当前设备/档位
  return true;
}

bool webrtc_engine::GetVideoDeviceCapabilities(const std::string& id) {
  if (!signaling_thread_) return false;
  if (signaling_thread_->IsCurrent()) return GetVideoDeviceCapabilitiesImpl(id);
  bool ok = false;
  signaling_thread_->BlockingCall(
      [&] { ok = GetVideoDeviceCapabilitiesImpl(id); });
  return ok;
}

bool webrtc_engine::GetVideoDeviceCapabilitiesImpl(const std::string& id) {
  webrtc_media::VideoDeviceInfo dev;
  if (!webrtc_media::FindVideoDevice(id, &dev)) {
    RTC_LOG(LS_WARNING) << "GetVideoDeviceCapabilities: not found: " << id;
    return false;
  }
  auto caps = webrtc_media::GetVideoDeviceCapabilities(dev);
  if (engine_observer_) {
    engine_observer_->OnVideoDeviceCapabilities(id, caps);
  }
  return true;
}

bool webrtc_engine::SetVideoProfile(int width, int height, int fps) {
  if (!signaling_thread_) return false;
  if (signaling_thread_->IsCurrent()) return SetVideoProfileImpl(width, height, fps);
  bool ok = false;
  signaling_thread_->BlockingCall(
      [&] { ok = SetVideoProfileImpl(width, height, fps); });
  return ok;
}

bool webrtc_engine::SetVideoProfileImpl(int width, int height, int fps) {
  // 目标设备：当前设备 > pending > 默认第一个设备。
  webrtc_media::VideoDeviceInfo dev;
  const std::string id =
      !current_device_id_.empty() ? current_device_id_ : pending_device_id_;
  if (id.empty() || !webrtc_media::FindVideoDevice(id, &dev)) {
    auto devices = webrtc_media::EnumerateVideoDevices();
    if (devices.empty()) {
      return false;
    }
    dev = devices.front();
  }
  // 校验：该设备的采集能力必须覆盖 尺寸+帧率。
  auto caps = webrtc_media::GetVideoDeviceCapabilities(dev);
  if (!webrtc_media::IsProfileSupported(caps, width, height, fps)) {
    RTC_LOG(LS_WARNING) << "SetVideoProfile: unsupported " << width << "x"
                        << height << "@" << fps << " on " << dev.id;
    return false;
  }

  if (!video_source_) {
    // 空闲：记住，下次建源生效。
    capture_width_ = width;
    capture_height_ = height;
    capture_fps_ = fps;
    capture_profile_explicit_ = true;
    RTC_LOG(LS_INFO) << "SetVideoProfile (idle): " << width << "x" << height
                     << "@" << fps;
    if (engine_observer_) {
      engine_observer_->OnVideoProfile(width, height, fps, true);
    }
    return true;
  }

  webrtc_media::VideoDeviceInfo target = dev;
  target.width = width;
  target.height = height;
  target.fps = fps;
  const bool ok =
      webrtc_media::SwitchVideoSourceDevice(video_source_.get(), target);
  if (ok) {
    capture_width_ = width;
    capture_height_ = height;
    capture_fps_ = fps;
    capture_profile_explicit_ = true;
  }
  RTC_LOG(LS_INFO) << "SetVideoProfile: " << width << "x" << height << "@" << fps
                   << " ok=" << ok;
  if (engine_observer_) {
    engine_observer_->OnVideoProfile(width, height, fps, ok);
  }
  return true;
}

// ============================================================================
//  周期性视频统计
// ============================================================================

void webrtc_engine::SetVideoStats(bool enabled, int interval_ms) {
  if (!signaling_thread_) return;
  if (signaling_thread_->IsCurrent()) { SetVideoStatsImpl(enabled, interval_ms); return; }
  signaling_thread_->BlockingCall([&] { SetVideoStatsImpl(enabled, interval_ms); });
}

void webrtc_engine::SetVideoStatsImpl(bool enabled, int interval_ms) {
  video_stats_enabled_ = enabled;
  if (interval_ms > 0) {
    video_stats_interval_ms_ = std::clamp(interval_ms, 500, 10000);
  }
  RTC_LOG(LS_INFO) << "SetVideoStats enabled=" << enabled
                   << " interval_ms=" << video_stats_interval_ms_;
  if (enabled) {
    ScheduleVideoStats();
  }
  GetVideoStatusImpl();
}

void webrtc_engine::ScheduleVideoStats() {
  if (!video_stats_enabled_ || !signaling_thread_) {
    return;
  }
  signaling_thread_->PostDelayedTask(
      [this] { CollectVideoStats(); },
      webrtc::TimeDelta::Millis(video_stats_interval_ms_));
}

void webrtc_engine::CollectVideoStats() {
  if (!video_stats_enabled_) {
    return;  // 已关闭，停止自续期
  }
  if (!peer_connection_ || !video_enabled_) {
    ScheduleVideoStats();  // 不在通话/视频关：稍后再试
    return;
  }
  // 续期放在发起侧：PC 重建时 GetStats 回调可能丢失（旧 PC 被销毁），
  // 若在回调里续期，定时链会断掉。
  ScheduleVideoStats();
  peer_connection_->GetStats(make_ref_counted<VideoStatsCollector>(this).get());
}

void webrtc_engine::VideoStatsCollector::OnStatsDelivered(
    const scoped_refptr<const RTCStatsReport>& report) {
  VideoStats stats;
  if (report) {
    for (const auto* s : report->GetStatsOfType<RTCOutboundRtpStreamStats>()) {
      if (s->kind && *s->kind == "video") {
        if (s->frame_width) stats.width = static_cast<int>(*s->frame_width);
        if (s->frame_height) stats.height = static_cast<int>(*s->frame_height);
        if (s->frames_per_second) stats.fps = *s->frames_per_second;
        if (s->target_bitrate) stats.target_bitrate = *s->target_bitrate;
        if (s->bytes_sent) stats.bytes_sent = *s->bytes_sent;
        if (s->packets_sent) stats.packets_sent = *s->packets_sent;
        if (s->frames_encoded) stats.frames_encoded = *s->frames_encoded;
        if (s->key_frames_encoded) stats.key_frames_encoded = *s->key_frames_encoded;
        if (s->quality_limitation_reason)
          stats.quality_limitation = *s->quality_limitation_reason;
        if (s->pli_count) stats.pli_count = *s->pli_count;
        if (s->nack_count) stats.nack_count = *s->nack_count;
        break;
      }
    }
    for (const auto* s : report->GetStatsOfType<RTCRemoteInboundRtpStreamStats>()) {
      if (s->kind && *s->kind == "video") {
        if (s->packets_lost) stats.packets_lost = *s->packets_lost;
        if (s->jitter) stats.jitter_ms = *s->jitter * 1000.0;
        break;
      }
    }
    // 活跃候选对（与 StatsCollector 同口径）。
    const RTCIceCandidatePairStats* active = nullptr;
    for (const auto* p : report->GetStatsOfType<RTCIceCandidatePairStats>()) {
      if (!active && p->writable && *p->writable) {
        active = p;
      }
      if (p->nominated && *p->nominated && p->writable && *p->writable) {
        active = p;
        break;
      }
    }
    if (active) {
      if (active->current_round_trip_time) {
        stats.rtt_ms = *active->current_round_trip_time * 1000.0;
      }
      if (active->available_outgoing_bitrate) {
        stats.available_outgoing_bitrate = *active->available_outgoing_bitrate;
      }
    }
  }
  if (engine_ && engine_->engine_observer_) {
    engine_->engine_observer_->OnVideoStats(stats);
  }
  // 下一次采集已在 CollectVideoStats 里预约，此处不再续期。
}

// ============================================================================
//  强制关键帧
// ============================================================================

bool webrtc_engine::RequestKeyFrame() {
  if (!signaling_thread_) return false;
  if (signaling_thread_->IsCurrent()) return RequestKeyFrameImpl();
  bool ok = false;
  signaling_thread_->BlockingCall([&] { ok = RequestKeyFrameImpl(); });
  return ok;
}

bool webrtc_engine::RequestKeyFrameImpl() {
  if (!video_sender_) {
    RTC_LOG(LS_WARNING) << "RequestKeyFrame: no video sender";
    if (engine_observer_) engine_observer_->OnKeyFrameRequested(false);
    return false;
  }
  const RTCError err = video_sender_->GenerateKeyFrame({});
  const bool ok = err.ok();
  RTC_LOG(LS_INFO) << "RequestKeyFrame ok=" << ok;
  if (engine_observer_) engine_observer_->OnKeyFrameRequested(ok);
  return ok;
}

// 热插拔：设备集合变化（在 signaling 线程执行）。
void webrtc_engine::OnVideoDevicesChangedImpl(
    const std::vector<webrtc_media::VideoDeviceInfo>& devices) {
  std::vector<std::string> ids;
  ids.reserve(devices.size());
  for (const auto& dev : devices) {
    ids.push_back(dev.id);
  }
  if (ids == known_device_ids_) {
    return;  // 无变化
  }

  // 判断是新增还是移除（双向比较；两者都有时以移除优先，因为它影响正在使用的设备）。
  bool device_added = false;
  bool device_removed = false;
  for (const auto& id : ids) {
    if (std::find(known_device_ids_.begin(), known_device_ids_.end(), id) ==
        known_device_ids_.end()) {
      device_added = true;
      break;
    }
  }
  for (const auto& id : known_device_ids_) {
    if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
      device_removed = true;
      break;
    }
  }

  // 当前正在使用的设备是否被拔出。
  const std::string previous_current = current_device_id_;
  const bool current_gone =
      !previous_current.empty() &&
      std::find(ids.begin(), ids.end(), previous_current) == ids.end();

  known_device_ids_ = ids;
  const std::string reason =
      device_removed ? "device_removed"
                     : (device_added ? "device_added" : "device_changed");

  if (current_gone) {
    RTC_LOG(LS_WARNING) << "Current video device removed: " << previous_current;
    // 关闭视频通道：移除视频轨并重新协商（对端会收到 remote_video.stopped）。
    if (peer_connection_ && video_enabled_) {
      VideoOffImpl();
    }
    // 采集源跟着设备一起失效，释放掉，下次 video_on 时按新设备重建。
    video_source_ = nullptr;
    video_track_ = nullptr;
    video_sender_ = nullptr;
    current_device_id_.clear();
    pending_device_id_.clear();
    capture_profile_explicit_ = false;
    if (engine_observer_) {
      engine_observer_->OnVideoDeviceLost(previous_current);
    }
    GetVideoStatusImpl();  // 回显：设备已清空、视频已关
  }

  if (engine_observer_) {
    engine_observer_->OnVideoDevicesChanged(devices, current_device_id_, reason);
  }
}

void webrtc_engine::SetBitrateImpl(int max_kbps) {
  video_bitrate_kbps_ = max_kbps;
  bool ok = ApplyRtpParameters();
  if (engine_observer_) engine_observer_->OnVideoSwitched(video_enabled_, ok);
  GetVideoStatusImpl();
}

void webrtc_engine::SetFramerateImpl(int fps) {
  video_fps_ = fps;
  bool ok = ApplyRtpParameters();
  if (engine_observer_) engine_observer_->OnVideoSwitched(video_enabled_, ok);
  GetVideoStatusImpl();
}

void webrtc_engine::SetDegradationImpl(const std::string& mode) {
  video_degradation_ = mode;
  bool ok = ApplyRtpParameters();
  if (engine_observer_) engine_observer_->OnVideoSwitched(video_enabled_, ok);
  GetVideoStatusImpl();
}

void webrtc_engine::GetVideoStatusImpl() {
  if (!engine_observer_) {
    return;
  }
  VideoState state;
  state.enabled = video_enabled_;
  state.in_call = (peer_connection_ != nullptr);
  state.device_id =
      !current_device_id_.empty() ? current_device_id_ : pending_device_id_;
  state.capture_width = capture_width_;
  state.capture_height = capture_height_;
  state.capture_fps = capture_fps_;
  state.encode_width = video_width_;
  state.encode_height = video_height_;
  state.encode_fps = video_fps_;
  state.encode_bitrate_kbps = video_bitrate_kbps_;
  state.encode_degradation = video_degradation_;

  if (!state.device_id.empty()) {
    webrtc_media::VideoDeviceInfo dev;
    if (webrtc_media::FindVideoDevice(state.device_id, &dev)) {
      state.device_name = dev.name;
      state.device_synthetic = dev.is_synthetic;
      // 实际生效的像素格式（按 WebRTC 的格式偏好 + 当前档位推出来）。
      auto caps = webrtc_media::GetVideoDeviceCapabilities(dev);
      state.capture_format = webrtc_media::EffectivePixelFormat(
          caps, state.capture_width, state.capture_height);
    }
  }
  engine_observer_->OnVideoState(state);
}

// 用当前 video_* 参数调 video_sender_->SetParameters（码率/帧率/降质）。
bool webrtc_engine::ApplyRtpParameters() {
  if (!video_sender_) return false;
  RtpParameters params = video_sender_->GetParameters();
  if (params.encodings.empty()) return false;
  if (video_bitrate_kbps_ > 0) {
    params.encodings[0].max_bitrate_bps = video_bitrate_kbps_ * 1000;
  }
  if (video_fps_ > 0) {
    params.encodings[0].max_framerate = video_fps_;
  }
  params.degradation_preference = StringToDegradation(video_degradation_);
  return video_sender_->SetParameters(params).ok();
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
  environment_ = CreateEnvironment(std::make_unique<FieldTrials>(""));
  PeerConnectionFactoryDependencies deps;
  deps.signaling_thread = signaling_thread_.get();
  deps.network_thread = network_thread_.get();
  deps.worker_thread = worker_thread_.get();
  deps.env = *environment_;
  // 真实平台 ADM（PulseAudio 后端），必须在 worker 线程创建并 Init；
  // 失败（没有音频服务）时回退 dummy，保证视频功能可用。
  adm_ = worker_thread_->BlockingCall(
      [this]() -> scoped_refptr<AudioDeviceModule> {
        auto adm = webrtc_media::CreatePlatformAudioDeviceModule(*environment_);
        if (!adm) {
          RTC_LOG(LS_WARNING) << "Platform ADM unavailable, fallback to dummy";
          adm = CreateAudioDeviceModule(*environment_,
                                        AudioDeviceModule::kDummyAudio);
          if (adm) adm->Init();
        }
        return adm;
      });
  deps.adm = adm_;
  // 音频热插拔基线：ADM 首次就绪时记录当前设备集合。
  if (!audio_devices_baseline_initialized_ && adm_) {
    const auto devices = worker_thread_->BlockingCall([this] {
      return webrtc_media::EnumerateRecordingDevices(adm_.get());
    });
    known_audio_device_ids_.clear();
    for (const auto& device : devices) {
      if (!IsDefaultAudioAlias(device.name)) {
        known_audio_device_ids_.insert(device.id.empty() ? device.name
                                                         : device.id);
      }
    }
    audio_devices_baseline_initialized_ = true;
  }
  // 注入「可捕获 APM 实例」的 builder，供运行时 AEC/NS/AGC 开关使用。
  webrtc_media::ConfigureAudioProcessing(deps, &apm_);
  // 软件静音：采集帧进入编码前清零（RTP 继续发静音包）。
  mic_muted_flag_ = std::make_shared<std::atomic<bool>>(mic_muted_);
  deps.audio_frame_processor =
      std::make_unique<webrtc_media::MuteFrameProcessor>(mic_muted_flag_);
  webrtc_media::ConfigureVideoCodecs(deps);
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

// 端口窗口轮换：WebRTC 的 BindSocket 每次都从 min_port 开始试绑，固定 min_port
// 会让每次呼叫都复用同一个本地端口；上一通失败时 TURN 释放包丢失，服务端旧分配
// 还占着这个 5-tuple，重呼就会一直 437。这里在配置范围内把端口切成若干组，每次
// 建 PC 换一组，连续呼叫就不会撞同一个 5-tuple。
void webrtc_engine::ApplyIcePortWindow() {
  const int base_min = ice_config_.min_port;
  const int base_max = ice_config_.max_port;
  if (base_min <= 0 || base_max <= base_min) {
    return;  // 未限制范围：沿用系统分配
  }
  const int total = base_max - base_min + 1;
  const int window = std::min(64, total);          // 每组 64 个端口（一通电话通常用 2~8 个）
  const int groups = std::max(1, total / window);  // 16384/64 = 256 组
  if (!ice_port_window_seeded_) {
    // 随机起点：避免进程重启后又从第 0 组开始，撞上上个进程残留的 TURN 分配。
    ice_port_window_index_ = static_cast<int>(std::random_device{}() % groups);
    ice_port_window_seeded_ = true;
  }
  const int index = ice_port_window_index_ % groups;
  const int start = base_min + index * window;
  const int end = std::min(start + window - 1, base_max);
  rtc_configuration_.set_min_port(start);
  rtc_configuration_.set_max_port(end);
  ice_port_window_index_ = (index + 1) % groups;
  RTC_LOG(LS_INFO) << "ICE port window: [" << start << "," << end << "] group "
                   << index << "/" << groups;
}

bool webrtc_engine::CreatePeerConnectionImpl() {
  if (peer_connection_) {
    return true;
  }
  // 每次新建 PC 换一组端口（连续呼叫不复用同一 5-tuple）。
  ApplyIcePortWindow();
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
  if (video_enabled_) {
    AddVideoTrackToPc();  // 建 offer 前加视频轨，offer 才含视频 m-line
  }
  if (audio_enabled_) {
    AddAudioTrackToPc();  // 同上：offer 才含音频 m-line
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
      std::move(desc), make_ref_counted<SetRemoteObserver>(this, true));
}

void webrtc_engine::SetRemoteAnswer(const std::string& sdp) {
  auto desc = CreateSessionDescription(SdpType::kAnswer, sdp);
  if (!desc) {
    RTC_LOG(LS_ERROR) << "Failed to parse remote answer";
    if (engine_observer_) engine_observer_->OnEngineError("set_remote_desc_failed", "failed to parse remote answer");
    return;
  }
  peer_connection_->SetRemoteDescription(
      std::move(desc), make_ref_counted<SetRemoteObserver>(this, false));
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
  pending_switch_mode_.reset();
  data_channel_ = nullptr;
  if (peer_connection_) {
    peer_connection_->Close();
    peer_connection_ = nullptr;
  }
  video_source_ = nullptr;   // 全拆：视频源也释放（frame generator 停）
  video_track_ = nullptr;
  video_sender_ = nullptr;
  remote_video_sink_.reset();
  // 音频对象都属于旧 factory/media engine；工厂销毁后必须全部释放，
  // 否则下一次 call 会复用已失效的 source/track/ADM 导致崩溃。
  audio_track_ = nullptr;
  audio_sender_ = nullptr;
  // 先把 ADM/APM 等移到局部，保证它们在 factory 销毁后仍存活。
  auto adm = std::move(adm_);
  auto apm = std::move(apm_);
  auto audio_source = std::move(audio_source_);
  auto muted_flag = std::move(mic_muted_flag_);
  // factory 仍在 signaling 线程销毁：factory 析构会等待 signaling 线程上的
  // 组件，放到 worker 上会和本次 BlockingCall 互相等待（死锁）。
  peer_connection_factory_ = nullptr;
  // ADM/APM 有 worker 线程亲和性，最后在 worker 上销毁。
  auto destroy_audio = [&adm, &apm, &audio_source, &muted_flag] {
    audio_source = nullptr;
    apm = nullptr;
    adm = nullptr;  // ADM 析构（worker）
    muted_flag = nullptr;
  };
  if (worker_thread_) {
    worker_thread_->BlockingCall(destroy_audio);
  } else {
    destroy_audio();
  }
  audio_devices_baseline_initialized_ = false;
  known_audio_device_ids_.clear();
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
  video_track_ = nullptr;   // track 随 PC 走，重建后重加（source 复用）
  video_sender_ = nullptr;
  audio_track_ = nullptr;   // 同上：音频轨也随 PC 走，source 复用
  audio_sender_ = nullptr;
  remote_video_sink_.reset();
  is_connected_ = false;
  data_channel_open_ = false;
}

// 释放旧 PC 后按本端当前 mode 重建 PC/DC 并发起新 offer（命令方用，对称）。
void webrtc_engine::RebuildAndOffer() {
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
  if (video_enabled_) {
    AddVideoTrackToPc();  // 重建后补回视频轨
  }
  if (audio_enabled_) {
    AddAudioTrackToPc();  // 重建后补回音频轨
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
  if (video_enabled_) {
    AddVideoTrackToPc();  // 建 answer 前加视频轨
  }
  if (audio_enabled_) {
    AddAudioTrackToPc();  // 建 answer 前加音频轨
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
  ReleasePeerConnection();  // 对端释放旧 PC（旧 TURN 分配），等 reoffer（对称）
  if (engine_observer_) {
    engine_observer_->OnSignalSwitchIce(from);
  }
}

void webrtc_engine::OnSignalReconnect(int from) {
  RTC_LOG(LS_INFO) << "OnSignalReconnect from=" << from;
  reconnect_in_progress_ = true;  // 对端只标记完成检测（命令方已 ICE 重启 + offer）
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
  if (video_enabled_ && !video_track_) {
    AddVideoTrackToPc();  // 重建后补回视频轨（answer 前）
  }
  if (audio_enabled_ && !audio_track_) {
    AddAudioTrackToPc();  // 重建后补回音频轨（answer 前）
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

void webrtc_engine::OnAddTrack(
    scoped_refptr<RtpReceiverInterface> receiver,
    const std::vector<scoped_refptr<MediaStreamInterface>>& streams) {
  auto track = receiver->track();
  if (!track) {
    return;
  }
  if (track->kind() == MediaStreamTrackInterface::kAudioKind) {
    RTC_LOG(LS_INFO) << "OnAddTrack: remote audio";
    if (engine_observer_) {
      engine_observer_->OnRemoteAudioStarted();
    }
    return;
  }
  if (track->kind() != MediaStreamTrackInterface::kVideoKind) {
    return;
  }
  RTC_LOG(LS_INFO) << "OnAddTrack: remote video";
  auto* video_track = static_cast<VideoTrackInterface*>(track.get());
  remote_video_sink_ = webrtc_media::CreateRemoteVideoFrameCounter();
  video_track->AddOrUpdateSink(remote_video_sink_.get(), VideoSinkWants());
  if (engine_observer_) {
    engine_observer_->OnRemoteVideoStarted();
  }
}

void webrtc_engine::OnRemoveTrack(scoped_refptr<RtpReceiverInterface> receiver) {
  auto track = receiver->track();
  if (!track) {
    return;
  }
  if (track->kind() == MediaStreamTrackInterface::kAudioKind) {
    RTC_LOG(LS_INFO) << "OnRemoveTrack: remote audio";
    if (engine_observer_) {
      engine_observer_->OnRemoteAudioStopped();
    }
    return;
  }
  if (track->kind() == MediaStreamTrackInterface::kVideoKind) {
    RTC_LOG(LS_INFO) << "OnRemoveTrack: remote video";
    remote_video_sink_.reset();
    if (engine_observer_) {
      engine_observer_->OnRemoteVideoStopped();
    }
  }
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
  if (state == PeerConnectionInterface::PeerConnectionState::kConnected) {
    MaybeRunPendingSwitch();
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
  // 对称重协商：以产出的 SDP 类型（offer/answer）决定发什么，而非 is_offerer_。
  const bool is_offer = (desc->GetType() == SdpType::kOffer);
  std::string type = is_offer ? "offer" : "answer";
  if (engine_->engine_observer_) {
    engine_->engine_observer_->OnLocalSdp(type, sdp);
  }
  engine_->peer_connection_->SetLocalDescription(
      std::unique_ptr<SessionDescriptionInterface>(desc),
      make_ref_counted<SetLocalObserver>(engine_));
  if (is_offer) {
    if (engine_->renegotiating_) {
      engine_->signaling_client_.SendReoffer(engine_->peer_id_, sdp);
    } else {
      engine_->signaling_client_.Call(engine_->peer_id_, sdp);
    }
  } else {
    engine_->signaling_client_.Answer(engine_->peer_id_, sdp);
    // 答方发出 answer 即 ICE 重启协商完成
    if (engine_->reconnect_in_progress_) {
      engine_->reconnect_in_progress_ = false;
      if (engine_->engine_observer_) engine_->engine_observer_->OnReconnected(true);
    } else if (engine_->video_switch_in_progress_) {
      engine_->video_switch_in_progress_ = false;
      if (engine_->engine_observer_) engine_->engine_observer_->OnVideoSwitched(engine_->video_enabled_, true);
    }
    // 音频与视频独立判断：同一次重协商可能同时完成两者。
    if (engine_->audio_switch_in_progress_) {
      engine_->audio_switch_in_progress_ = false;
      if (engine_->engine_observer_) engine_->engine_observer_->OnAudioSwitched(engine_->audio_enabled_, true);
    }
  }
  engine_->MaybeRunPendingSwitch();
}

void webrtc_engine::CreateSdpObserver::OnFailure(RTCError error) {
  RTC_LOG(LS_ERROR) << "CreateOffer/Answer failed: " << error.message();
  if (engine_->engine_observer_) engine_->engine_observer_->OnEngineError("sdp_create_failed", error.message());
  if (engine_->reconnect_in_progress_) {
    engine_->reconnect_in_progress_ = false;
    if (engine_->engine_observer_) engine_->engine_observer_->OnReconnected(false);
  } else if (engine_->video_switch_in_progress_) {
    engine_->video_switch_in_progress_ = false;
    if (engine_->engine_observer_) engine_->engine_observer_->OnVideoSwitched(engine_->video_enabled_, false);
  }
  if (engine_->audio_switch_in_progress_) {
    engine_->audio_switch_in_progress_ = false;
    if (engine_->engine_observer_) engine_->engine_observer_->OnAudioSwitched(engine_->audio_enabled_, false);
  }
  engine_->MaybeRunPendingSwitch();
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
    if (is_offer_) {
      // 收的是 offer → 生成 answer
      engine_->CreateAnswer();
    } else if (engine_->reconnect_in_progress_) {
      // 收的是 answer（ICE 重启协商完成）
      engine_->reconnect_in_progress_ = false;
      if (engine_->engine_observer_) engine_->engine_observer_->OnReconnected(true);
    } else if (engine_->video_switch_in_progress_) {
      // 收的是 answer（视频开关重协商完成）
      engine_->video_switch_in_progress_ = false;
      if (engine_->engine_observer_) engine_->engine_observer_->OnVideoSwitched(engine_->video_enabled_, true);
    }
    if (engine_->audio_switch_in_progress_) {
      // 收的是 answer（音频开关重协商完成）
      engine_->audio_switch_in_progress_ = false;
      if (engine_->engine_observer_) engine_->engine_observer_->OnAudioSwitched(engine_->audio_enabled_, true);
    }
    engine_->MaybeRunPendingSwitch();
  } else {
    RTC_LOG(LS_ERROR) << "SetRemoteDescription failed: " << error.message();
    if (engine_->engine_observer_) engine_->engine_observer_->OnEngineError("set_remote_desc_failed", error.message());
    if (engine_->reconnect_in_progress_) {
      engine_->reconnect_in_progress_ = false;
      if (engine_->engine_observer_) engine_->engine_observer_->OnReconnected(false);
    } else if (engine_->video_switch_in_progress_) {
      engine_->video_switch_in_progress_ = false;
      if (engine_->engine_observer_) engine_->engine_observer_->OnVideoSwitched(engine_->video_enabled_, false);
    }
    if (engine_->audio_switch_in_progress_) {
      engine_->audio_switch_in_progress_ = false;
      if (engine_->engine_observer_) engine_->engine_observer_->OnAudioSwitched(engine_->audio_enabled_, false);
    }
    engine_->MaybeRunPendingSwitch();
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
