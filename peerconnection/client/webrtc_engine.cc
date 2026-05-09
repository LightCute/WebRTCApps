/*
 *  Copyright 2026 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "apps/peerconnection/client/webrtc_engine.h"
#include "apps/peerconnection/client/json_helpers.h"
#include "apps/peerconnection/client/pc_factory.h"
#include "apps/peerconnection/client/peer_connection_client.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunsafe-buffer-usage"
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"

#include "absl/flags/declare.h"
#include "absl/flags/flag.h"

ABSL_DECLARE_FLAG(std::string, audio_source);

#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/memory/memory.h"
#include "api/audio/audio_device_defines.h"
#include "api/audio/create_audio_device_module.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/audio_options.h"
#include "api/create_modular_peer_connection_factory.h"
#include "api/enable_media.h"
#include "api/jsep.h"
#include "api/make_ref_counted.h"
#include "api/media_stream_interface.h"
#include "api/peer_connection_interface.h"
#include "api/rtc_error.h"
#include "api/rtp_receiver_interface.h"
#include "api/rtp_sender_interface.h"
#include "api/scoped_refptr.h"
#include "api/task_queue/task_queue_factory.h"
#include "api/test/create_frame_generator.h"
#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"
#include "api/video/video_source_interface.h"
#include "api/video_codecs/video_decoder_factory_template.h"
#include "api/video_codecs/video_decoder_factory_template_dav1d_adapter.h"
#include "api/video_codecs/video_decoder_factory_template_libvpx_vp8_adapter.h"
#include "api/video_codecs/video_decoder_factory_template_libvpx_vp9_adapter.h"
#include "api/video_codecs/video_decoder_factory_template_open_h264_adapter.h"
#include "api/video_codecs/video_encoder_factory_template.h"
#include "api/video_codecs/video_encoder_factory_template_libaom_av1_adapter.h"
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp8_adapter.h"
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp9_adapter.h"
#include "api/video_codecs/video_encoder_factory_template_open_h264_adapter.h"
#include "apps/peerconnection/client/defaults.h"
#include "apps/peerconnection/client/shm_audio_writer.h"
#include "apps/peerconnection/client/shm_video_writer.h"
#include "common_video/libyuv/include/webrtc_libyuv.h"
#include "json/json.h"
#include "modules/video_capture/video_capture.h"
#include "modules/video_capture/video_capture_factory.h"
#include "pc/video_track_source.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/strings/json.h"
#include "rtc_base/thread.h"
#include "system_wrappers/include/clock.h"
#include "test/frame_generator_capturer.h"
#include "test/platform_video_capturer.h"
#include "test/test_video_capturer.h"

namespace {

using webrtc::test::TestVideoCapturer;

class DummySetSessionDescriptionObserver
    : public webrtc::SetSessionDescriptionObserver {
 public:
  static webrtc::scoped_refptr<DummySetSessionDescriptionObserver> Create() {
    return webrtc::make_ref_counted<DummySetSessionDescriptionObserver>();
  }
  void OnSuccess() override { RTC_LOG(LS_INFO) << __FUNCTION__; }
  void OnFailure(webrtc::RTCError error) override {
    RTC_LOG(LS_INFO) << __FUNCTION__ << " " << ToString(error.type()) << ": "
                     << error.message();
  }
};

std::unique_ptr<TestVideoCapturer> CreateCapturer(
    webrtc::TaskQueueFactory& task_queue_factory,
    int device_idx = -1) {
  const size_t kWidth = 640;
  const size_t kHeight = 480;
  const size_t kFps = 30;
  std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> info(
      webrtc::VideoCaptureFactory::CreateDeviceInfo());
  if (info) {
    int num_devices = info->NumberOfDevices();
    if (device_idx >= 0 && device_idx < num_devices) {
      std::unique_ptr<TestVideoCapturer> capturer =
          webrtc::test::CreateVideoCapturer(kWidth, kHeight, kFps, device_idx);
      if (capturer) {
        return capturer;
      }
    }
    for (int i = 0; i < num_devices; ++i) {
      std::unique_ptr<TestVideoCapturer> capturer =
          webrtc::test::CreateVideoCapturer(kWidth, kHeight, kFps, i);
      if (capturer) {
        return capturer;
      }
    }
  }
  RTC_LOG(LS_WARNING)
      << "No video capture device found; using synthetic video.";
  auto frame_generator = webrtc::test::CreateSquareFrameGenerator(
      kWidth, kHeight, std::nullopt, std::nullopt);
  return std::make_unique<webrtc::test::FrameGeneratorCapturer>(
      webrtc::Clock::GetRealTimeClock(), std::move(frame_generator), kFps,
      task_queue_factory);
}

class CapturerTrackSource : public webrtc::VideoTrackSource {
 public:
  static webrtc::scoped_refptr<CapturerTrackSource> Create(
      webrtc::TaskQueueFactory& task_queue_factory,
      int device_idx = -1) {
    std::unique_ptr<TestVideoCapturer> capturer =
        CreateCapturer(task_queue_factory, device_idx);
    if (capturer) {
      capturer->Start();
      return webrtc::make_ref_counted<CapturerTrackSource>(std::move(capturer));
    }
    return nullptr;
  }

  void SwapCapturer(std::unique_ptr<TestVideoCapturer> new_capturer) {
    if (!new_capturer) return;
    RTC_LOG(LS_INFO) << "Swapping video capturer";
    {
      std::lock_guard<std::mutex> lock(capturer_mutex_);
      capturer_->Stop();
      capturer_ = std::move(new_capturer);
      capturer_->Start();
    }
  }

 protected:
  explicit CapturerTrackSource(std::unique_ptr<TestVideoCapturer> capturer)
      : VideoTrackSource(/*remote=*/false), capturer_(std::move(capturer)) {}

  ~CapturerTrackSource() override = default;

 private:
  webrtc::VideoSourceInterface<webrtc::VideoFrame>* source() override {
    return capturer_.get();
  }

  std::mutex capturer_mutex_;
  std::unique_ptr<TestVideoCapturer> capturer_;
};

}  // namespace

// ==================== WebRTCEngine ====================

WebRTCEngine::WebRTCEngine(const webrtc::Environment& env)
    : env_(env),
      safety_(webrtc::PendingTaskSafetyFlag::Create()),
      peer_id_(-1),
      loopback_(false) {
  signaling_ = std::make_unique<PeerConnectionClient>();
  signaling_->RegisterObserver(this);
}

void WebRTCEngine::RegisterObserver(EngineObserver* observer) {
  if (signaling_thread_->IsCurrent()) {
    observer_ = observer;
  } else {
    signaling_thread_->PostTask([this, observer] { observer_ = observer; });
  }
}

void WebRTCEngine::UnregisterObserver() {
  if (signaling_thread_->IsCurrent()) {
    observer_ = nullptr;
  } else {
    signaling_thread_->PostTask([this] { observer_ = nullptr; });
  }
}

WebRTCEngine::~WebRTCEngine() {
  RTC_DCHECK(!peer_connection_);

  if (pipeline_)
    pipeline_->Shutdown();

  // Stop all threads.
  if (signaling_thread_) {
    signaling_thread_->Stop();
  }
  if (worker_thread_) {
    worker_thread_->Stop();
  }
  if (network_thread_) {
    network_thread_->Stop();
  }
}

bool WebRTCEngine::Init() {
  // Create threads.
  if (!signaling_thread_) {
    signaling_thread_ = webrtc::Thread::CreateWithSocketServer();
    signaling_thread_->Start();
  }
  if (!worker_thread_) {
    worker_thread_ = webrtc::Thread::Create();
    worker_thread_->Start();
  }
  if (!network_thread_) {
    network_thread_ = webrtc::Thread::CreateWithSocketServer();
    network_thread_->Start();
  }
  return true;
}

void WebRTCEngine::Shutdown() {
  signaling_->SignOut();
  DeletePeerConnection();

  // Clean up pending messages.
  while (!pending_messages_.empty()) {
    delete pending_messages_.front();
    pending_messages_.pop_front();
  }

  // Stop SHM renderers.
  pipeline_->StopLocalRenderer();
  pipeline_->StopRemoteRenderer();
  pipeline_->StopRemoteAudioRenderer();
  local_audio_source_ = nullptr;

  if (pipeline_)
    pipeline_->Shutdown();

  // Stop all threads.
  if (signaling_thread_) {
    signaling_thread_->Stop();
    signaling_thread_.reset();
  }
  if (worker_thread_) {
    worker_thread_->Stop();
    worker_thread_.reset();
  }
  if (network_thread_) {
    network_thread_->Stop();
    network_thread_.reset();
  }
}

void WebRTCEngine::ConnectToServer(const std::string& server, int port) {
  if (signaling_thread_->IsCurrent()) {
    ConnectToServerImpl(server, port);
  } else {
    signaling_thread_->PostTask(
        [this, server, port] { ConnectToServerImpl(server, port); });
  }
}

void WebRTCEngine::ConnectToServerImpl(const std::string& server, int port) {
  if (signaling_->is_connected())
    return;
  server_ = server;
  server_port_ = port;
  signaling_->Connect(server, port, GetPeerName());
}

void WebRTCEngine::DisconnectFromServer() {
  if (signaling_thread_->IsCurrent()) {
    DisconnectFromServerImpl();
  } else {
    signaling_thread_->PostTask([this] { DisconnectFromServerImpl(); });
  }
}

void WebRTCEngine::DisconnectFromServerImpl() {
  if (signaling_->is_connected())
    signaling_->SignOut();
}

void WebRTCEngine::ConnectToPeer(int peer_id) {
  if (signaling_thread_->IsCurrent()) {
    ConnectToPeerImpl(peer_id);
  } else {
    signaling_thread_->PostTask([this, peer_id] { ConnectToPeerImpl(peer_id); });
  }
}

void WebRTCEngine::ConnectToPeerImpl(int peer_id) {
  RTC_DCHECK(peer_id_ == -1);
  RTC_DCHECK(peer_id != -1);

  if (peer_connection_) {
    RTC_LOG(LS_ERROR) << "We only support connecting to one peer at a time";
    return;
  }

  if (InitializePeerConnection()) {
    peer_id_ = peer_id;
    peer_connection_->CreateOffer(
        this, webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
  } else {
    RTC_LOG(LS_ERROR) << "Failed to initialize PeerConnection";
  }
}

void WebRTCEngine::HangUp() {
  if (signaling_thread_->IsCurrent()) {
    HangUpImpl();
  } else {
    signaling_thread_->PostTask([this] { HangUpImpl(); });
  }
}

void WebRTCEngine::HangUpImpl() {
  RTC_LOG(LS_INFO) << __FUNCTION__;
  if (peer_connection_) {
    // Server-mediated hangup: send /hangup to server, which sends
    // HANGUP_CONFIRM to both parties. Cleanup happens in OnPeerDisconnected
    // when HANGUP_CONFIRM arrives.
    signaling_->SendHangUp(peer_id_);
  }
  //on_event_(BuildPeerListJson(signaling_->peers()));
}

void WebRTCEngine::SetAudioMuted(bool muted) {
  if (signaling_thread_->IsCurrent()) {
    SetAudioMutedImpl(muted);
  } else {
    signaling_thread_->PostTask([this, muted] { SetAudioMutedImpl(muted); });
  }
}

void WebRTCEngine::SetAudioMutedImpl(bool muted) {
  // Stub: log and set internal state. Full implementation can follow.
  RTC_LOG(LS_INFO) << "SetAudioMuted: " << (muted ? "true" : "false");
  if (pipeline_ && pipeline_->adm()) {
    if (muted) {
      pipeline_->adm()->StopRecording();
    }
    // Resume recording on unmute is left as a future enhancement.
  }
}

void WebRTCEngine::SetVideoPaused(bool paused) {
  if (signaling_thread_->IsCurrent()) {
    SetVideoPausedImpl(paused);
  } else {
    signaling_thread_->PostTask([this, paused] { SetVideoPausedImpl(paused); });
  }
}

void WebRTCEngine::SetVideoPausedImpl(bool paused) {
  // Stub: log and set internal state. Full implementation can follow.
  RTC_LOG(LS_INFO) << "SetVideoPaused: " << (paused ? "true" : "false");
  if (peer_connection_) {
    auto senders = peer_connection_->GetSenders();
    for (auto& sender : senders) {
      if (sender->track() &&
          sender->track()->kind() ==
              webrtc::MediaStreamTrackInterface::kVideoKind) {
        sender->track()->set_enabled(!paused);
      }
    }
  }
}

void WebRTCEngine::SendData(const std::string& text) {
  if (signaling_thread_->IsCurrent()) {
    SendDataImpl(text);
  } else {
    signaling_thread_->PostTask([this, text] { SendDataImpl(text); });
  }
}

void WebRTCEngine::SendDataImpl(const std::string& text) {
  dc_manager_->Send(text);
}

// ==================== Device Management ====================

void WebRTCEngine::QueryDevices() {
  if (signaling_thread_->IsCurrent()) {
    QueryDevicesImpl();
  } else {
    signaling_thread_->PostTask([this] { QueryDevicesImpl(); });
  }
}

void WebRTCEngine::QueryDevicesImpl() {
  // ADM is created by InitializePeerConnection() when a call starts.
  // Don't create it here — PulseAudio init crashes in some environments.
  if (!pipeline_ || (!pipeline_->adm() && worker_thread_)) {
    // Skip ADM creation; audio device list will be empty until first call.
  }

  Json::Value video_arr(Json::arrayValue);
  auto info = webrtc::VideoCaptureFactory::CreateDeviceInfo();
  if (info) {
    int n = info->NumberOfDevices();
    char name[256];
    char id[256];
    for (int i = 0; i < n; ++i) {
      if (info->GetDeviceName(i, name, sizeof(name), id, sizeof(id)) == 0) {
        Json::Value dev;
        dev["idx"] = i;
        dev["name"] = name;
        video_arr.append(dev);
      }
    }
  }
  Json::StreamWriterBuilder factory;
  factory["indentation"] = "";
  Json::Value video_event;
  video_event["event"] = "video_devices";
  video_event["devices"] = video_arr;
  if (observer_) observer_->OnEngineEvent(Json::writeString(factory, video_event));

  Json::Value audio_arr(Json::arrayValue);
  if (pipeline_ && pipeline_->adm() && worker_thread_) {
    audio_arr = worker_thread_->BlockingCall([this]() -> Json::Value {
      Json::Value arr(Json::arrayValue);
      int16_t n = pipeline_->adm()->RecordingDevices();
      char name[webrtc::kAdmMaxDeviceNameSize];
      char guid[webrtc::kAdmMaxGuidSize];
      for (int16_t i = 0; i < n; ++i) {
        if (pipeline_->adm()->RecordingDeviceName(i, name, guid) == 0) {
          Json::Value dev;
          dev["idx"] = i;
          dev["name"] = name;
          arr.append(dev);
        }
      }
      return arr;
    });
  }
  // Only emit ADM results if non-empty; otherwise fall through to ALSA
  if (!audio_arr.empty()) {
    Json::Value audio_event;
    audio_event["event"] = "audio_input_devices";
    audio_event["devices"] = audio_arr;
    if (observer_) observer_->OnEngineEvent(Json::writeString(factory, audio_event));
  }

  // ALSA fallback: use arecord -l when ADM enumeration returns empty.
  // ADM RecordingDevices requires InitRecording which crashes PulseAudio
  // in some environments (safe_conversions overflow).
  if (audio_arr.empty()) {
    FILE* fp = popen("arecord -l 2>/dev/null", "r");
    if (fp) {
      Json::Value alsa_arr(Json::arrayValue);
      char line[256];
      int idx = 0;
      while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "card ") == line && strstr(line, "device ")) {
          char* desc_begin = strrchr(line, '[');
          char* desc_end = desc_begin ? strrchr(line, ']') : nullptr;
          char name[256];
          if (desc_begin && desc_end && desc_end > desc_begin) {
            size_t len = desc_end - desc_begin - 1;
            snprintf(name, sizeof(name), "%.*s", (int)len, desc_begin + 1);
          } else {
            snprintf(name, sizeof(name), "Capture device %d", idx);
          }
          Json::Value dev;
          dev["idx"] = idx;
          dev["name"] = name;
          alsa_arr.append(dev);
          idx++;
        }
      }
      pclose(fp);
      if (!alsa_arr.empty()) {
        Json::Value alsa_event;
        alsa_event["event"] = "audio_input_devices";
        alsa_event["devices"] = alsa_arr;
        if (observer_) observer_->OnEngineEvent(Json::writeString(factory, alsa_event));
      }
    }
  }
}

void WebRTCEngine::SetVideoDevice(int device_idx) {
  if (signaling_thread_->IsCurrent()) {
    SetVideoDeviceImpl(device_idx);
  } else {
    signaling_thread_->PostTask([this, device_idx] { SetVideoDeviceImpl(device_idx); });
  }
}

void WebRTCEngine::SetVideoDeviceImpl(int device_idx) {
  if (!local_video_source_) {
    RTC_LOG(LS_WARNING) << "No local video source to swap";
    return;
  }
  auto new_capturer = CreateCapturer(env_.task_queue_factory(), device_idx);
  if (!new_capturer) {
    RTC_LOG(LS_ERROR) << "Failed to create capturer for device " << device_idx;
    return;
  }
  auto* capturer_source = static_cast<CapturerTrackSource*>(local_video_source_.get());
  capturer_source->SwapCapturer(std::move(new_capturer));
  if (pipeline_) pipeline_->set_video_device_idx(device_idx);
}

void WebRTCEngine::SetAudioInputDevice(int device_idx) {
  if (signaling_thread_->IsCurrent()) {
    SetAudioInputDeviceImpl(device_idx);
  } else {
    signaling_thread_->PostTask([this, device_idx] { SetAudioInputDeviceImpl(device_idx); });
  }
}

void WebRTCEngine::SetAudioInputDeviceImpl(int device_idx) {
  if (pipeline_) pipeline_->set_audio_input_device_idx(device_idx);
  if (!pipeline_ || !pipeline_->adm()) return;
  // Apply device change. If not recording yet, recording will use this device
  // when it starts via the factory. If already recording mid-call, restart.
  worker_thread_->BlockingCall([this, device_idx] {
    bool was_recording = pipeline_->adm()->Recording();
    if (was_recording) {
      pipeline_->adm()->StopRecording();
      pipeline_->adm()->SetRecordingDevice(device_idx);
      pipeline_->adm()->InitRecording();
      pipeline_->adm()->StartRecording();
    } else {
      pipeline_->adm()->SetRecordingDevice(device_idx);
    }
  });
}

bool WebRTCEngine::connection_active() const {
  return connection_active_.load(std::memory_order_acquire);
}

// ==================== PeerConnectionObserver ====================

void WebRTCEngine::OnAddTrack(
    webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
    const std::vector<webrtc::scoped_refptr<webrtc::MediaStreamInterface>>&
        streams) {
  RTC_LOG(LS_INFO) << __FUNCTION__ << " " << receiver->id();

  auto* track = receiver->track().get();
  if (track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
    auto* video_track = static_cast<webrtc::VideoTrackInterface*>(track);
    pipeline_->StartRemoteRenderer(video_track);
  } else if (track->kind() == webrtc::MediaStreamTrackInterface::kAudioKind) {
    auto* audio_track = static_cast<webrtc::AudioTrackInterface*>(track);
    pipeline_->StartRemoteAudioRenderer(audio_track);
  }
}

void WebRTCEngine::OnRemoveTrack(
    webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) {
  RTC_LOG(LS_INFO) << __FUNCTION__ << " " << receiver->id();

  auto* track = receiver->track().get();
  if (track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
    pipeline_->StopRemoteRenderer();
  } else if (track->kind() == webrtc::MediaStreamTrackInterface::kAudioKind) {
    pipeline_->StopRemoteAudioRenderer();
  }
}

void WebRTCEngine::OnDataChannel(
    webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) {
  dc_manager_->OnRemoteDataChannel(channel);
}

void WebRTCEngine::OnIceConnectionChange(
    webrtc::PeerConnectionInterface::IceConnectionState new_state) {
  RTC_LOG(LS_INFO) << __FUNCTION__ << " " << new_state;
  if (observer_) observer_->OnEngineEvent(std::string(R"({"event":"ice_state","state":")") +
            IceConnectionStateToString(new_state) + R"("})");
}

void WebRTCEngine::OnIceCandidate(const webrtc::IceCandidate* candidate) {
  RTC_LOG(LS_INFO) << __FUNCTION__ << " " << candidate->sdp_mline_index();

  Json::Value jmessage;
  jmessage[kCandidateSdpMidName] = candidate->sdp_mid();
  jmessage[kCandidateSdpMlineIndexName] = candidate->sdp_mline_index();
  jmessage[kCandidateSdpName] = candidate->ToString();

  Json::StreamWriterBuilder factory;
  SendMessage(Json::writeString(factory, jmessage));
}

// ==================== CreateSessionDescriptionObserver ====================

void WebRTCEngine::OnSuccess(webrtc::SessionDescriptionInterface* desc) {
  peer_connection_->SetLocalDescription(
      DummySetSessionDescriptionObserver::Create().get(), desc);

  std::string sdp;
  desc->ToString(&sdp);

  Json::Value jmessage;
  jmessage[kSessionDescriptionTypeName] =
      webrtc::SdpTypeToString(desc->GetType());
  jmessage[kSessionDescriptionSdpName] = sdp;

  Json::StreamWriterBuilder factory;
  SendMessage(Json::writeString(factory, jmessage));
}

void WebRTCEngine::OnFailure(webrtc::RTCError error) {
  RTC_LOG(LS_ERROR) << ToString(error.type()) << ": " << error.message();
}

// ==================== PeerConnectionClientObserver ====================

void WebRTCEngine::OnSignedIn() {
  RTC_LOG(LS_INFO) << __FUNCTION__;
  if (observer_) observer_->OnEngineEvent(R"({"event":"server_connected"})");
  // Also emit current peer list.
  const Peers& peers = signaling_->peers();
  if (!peers.empty()) {
    if (observer_) observer_->OnEngineEvent(BuildPeerListJson(peers));
  }
}

void WebRTCEngine::OnDisconnected() {
  RTC_LOG(LS_INFO) << __FUNCTION__;

  DeletePeerConnection();

  if (observer_) observer_->OnEngineEvent(R"({"event":"server_disconnected"})");
}

void WebRTCEngine::OnPeerConnected(int id, const std::string& name) {
  RTC_LOG(LS_INFO) << __FUNCTION__;
  // Emit individual peer event + full peer list.
  if (observer_) observer_->OnEngineEvent(std::string(R"({"event":"peer_online","peer":{"id":)") +
            std::to_string(id) + R"(,"name":")" + name + R"("}})");
  if (observer_) observer_->OnEngineEvent(BuildPeerListJson(signaling_->peers()));
}

void WebRTCEngine::OnPeerDisconnected(int id) {
  RTC_LOG(LS_INFO) << __FUNCTION__;
  if (id == peer_id_) {
    RTC_LOG(LS_INFO) << "Our peer disconnected";
    // Phase 2 confirmation — must be sent before cleanup.
    signaling_->SendHangUpConfirm();
    if (observer_) observer_->OnEngineEvent(R"({"event":"call_disconnected"})");
    if (observer_) observer_->OnEngineEvent(BuildPeerListJson(signaling_->peers()));

    // Sign out + reconnect: fully reset signaling state so the second
    // call starts from a clean slate (no residual call_partner / hangup
    // state on the server side).
    std::string saved_server = server_;
    int saved_port = server_port_;
    auto pc = std::move(peer_connection_);
    auto f = std::move(factory_);
    auto vs = std::move(local_video_source_);
    auto adm = pipeline_->adm();
    auto las = std::move(local_audio_source_);
    pipeline_->StopLocalRenderer();
    pipeline_->StopRemoteRenderer();
    pipeline_->StopRemoteAudioRenderer();
    dc_manager_->Shutdown();
    peer_id_ = -1;
    loopback_ = false;
    signaling_->Close();
    signaling_thread_->PostTask([this, pc = std::move(pc), f = std::move(f),
                                  vs = std::move(vs), adm = std::move(adm),
                                  las = std::move(las)]() mutable {
      while (!pending_messages_.empty()) {
        delete pending_messages_.front();
        pending_messages_.pop_front();
      }
      // Stop ADM asynchronously — the worker thread handles it.
      if (adm && worker_thread_) {
        worker_thread_->PostTask([adm]() mutable {
          if (adm->Playing()) adm->StopPlayout();
          if (adm->Recording()) adm->StopRecording();
          adm = nullptr;
        });
      }
      pc->Close();
      pc = nullptr;
      f = nullptr;
      vs = nullptr;
      las = nullptr;
    });
    // Auto-reconnect after cleanup (server has removed the old member entry).
    signaling_thread_->PostDelayedTask(
        [this, saved_server, saved_port] {
          RTC_LOG(LS_INFO) << "Auto-reconnecting after hangup to "
                           << saved_server << ":" << saved_port;
          ConnectToServer(saved_server, saved_port);
        },
        webrtc::TimeDelta::Seconds(2));
  } else {
    // Emit individual offline event + refreshed peer list.
    if (observer_) observer_->OnEngineEvent(R"({"event":"peer_offline","peer_id":)" +
              std::to_string(id) + "}");
    if (observer_) observer_->OnEngineEvent(BuildPeerListJson(signaling_->peers()));
  }
}

void WebRTCEngine::OnPeerBusy(int peer_id) {
  RTC_LOG(LS_INFO) << __FUNCTION__ << " " << peer_id;
  if (observer_) observer_->OnEngineEvent(std::string(R"({"event":"peer_busy","peer_id":)")
            + std::to_string(peer_id) + "}");
}

void WebRTCEngine::OnMessageFromPeer(int peer_id,
                                      const std::string& message) {
  RTC_DCHECK(peer_id_ == peer_id || peer_id_ == -1);
  RTC_DCHECK(!message.empty());

  if (!peer_connection_) {
    RTC_DCHECK(peer_id_ == -1);
    peer_id_ = peer_id;

    if (!InitializePeerConnection()) {
      RTC_LOG(LS_ERROR) << "Failed to initialize our PeerConnection instance";
      signaling_->SignOut();
      return;
    }
  } else if (peer_id != peer_id_) {
    RTC_DCHECK(peer_id_ != -1);
    RTC_LOG(LS_WARNING)
        << "Received a message from unknown peer while already in a "
           "conversation with a different peer.";
    return;
  }

  Json::CharReaderBuilder factory;
  std::unique_ptr<Json::CharReader> reader =
      absl::WrapUnique(factory.newCharReader());
  Json::Value jmessage;
  if (!reader->parse(message.data(), message.data() + message.length(),
                     &jmessage, nullptr)) {
    RTC_LOG(LS_WARNING) << "Received unknown message. " << message;
    return;
  }
  std::string type_str;
  std::string json_object;

  webrtc::GetStringFromJsonObject(jmessage, kSessionDescriptionTypeName,
                                  &type_str);
  if (!type_str.empty()) {
    std::optional<webrtc::SdpType> type_maybe =
        webrtc::SdpTypeFromString(type_str);
    if (!type_maybe) {
      RTC_LOG(LS_ERROR) << "Unknown SDP type: " << type_str;
      return;
    }
    webrtc::SdpType type = *type_maybe;
    std::string sdp;
    if (!webrtc::GetStringFromJsonObject(jmessage, kSessionDescriptionSdpName,
                                         &sdp)) {
      RTC_LOG(LS_WARNING)
          << "Can't parse received session description message.";
      return;
    }
    webrtc::SdpParseError error;
    std::unique_ptr<webrtc::SessionDescriptionInterface> session_description =
        webrtc::CreateSessionDescription(type, sdp, &error);
    if (!session_description) {
      RTC_LOG(LS_WARNING)
          << "Can't parse received session description message. "
             "SdpParseError was: "
          << error.description;
      return;
    }
    RTC_LOG(LS_INFO) << " Received session description :" << message;
    peer_connection_->SetRemoteDescription(
        DummySetSessionDescriptionObserver::Create().get(),
        session_description.release());
    if (type == webrtc::SdpType::kOffer) {
      peer_connection_->CreateAnswer(
          this, webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
    }
  } else {
    std::string sdp_mid;
    int sdp_mlineindex = 0;
    std::string sdp;
    if (!webrtc::GetStringFromJsonObject(jmessage, kCandidateSdpMidName,
                                         &sdp_mid) ||
        !webrtc::GetIntFromJsonObject(jmessage, kCandidateSdpMlineIndexName,
                                      &sdp_mlineindex) ||
        !webrtc::GetStringFromJsonObject(jmessage, kCandidateSdpName, &sdp)) {
      RTC_LOG(LS_WARNING) << "Can't parse received message.";
      return;
    }
    webrtc::SdpParseError error;
    std::unique_ptr<webrtc::IceCandidate> candidate(
        webrtc::CreateIceCandidate(sdp_mid, sdp_mlineindex, sdp, &error));
    if (!candidate) {
      RTC_LOG(LS_WARNING) << "Can't parse received candidate message. "
                             "SdpParseError was: "
                          << error.description;
      return;
    }
    if (!peer_connection_->AddIceCandidate(candidate.get())) {
      RTC_LOG(LS_WARNING) << "Failed to apply the received candidate";
      return;
    }
    RTC_LOG(LS_INFO) << " Received candidate :" << message;
  }
}

void WebRTCEngine::OnMessageSent(int err) {
  // Process the next pending message if any.
  // This is the inline version of the conductor's SEND_MESSAGE_TO_PEER callback.

  RTC_LOG(LS_INFO) << "OnMessageSent";

  if (!pending_messages_.empty() && !signaling_->IsSendingMessage()) {
    std::string* msg = pending_messages_.front();
    pending_messages_.pop_front();

    if (!signaling_->SendToPeer(peer_id_, *msg) && peer_id_ != -1) {
      RTC_LOG(LS_ERROR) << "SendToPeer failed";
      DisconnectFromServer();
    }
    delete msg;
  }

  if (!peer_connection_)
    peer_id_ = -1;

  // If a hangup was deferred because the control socket was busy, send it now
  if (pending_hangup_peer_id_ != -1 && !signaling_->IsSendingMessage()) {
    RTC_LOG(LS_INFO) << "Sending deferred BYE to peer " << pending_hangup_peer_id_;
    signaling_->SendHangUp(pending_hangup_peer_id_);
    pending_hangup_peer_id_ = -1;
  }
}

void WebRTCEngine::OnServerConnectionFailure() {
  std::string error_msg = "Failed to connect to " + server_;
  RTC_LOG(LS_ERROR) << error_msg;
  if (observer_) observer_->OnEngineEvent(R"({"event":"server_connection_failed","error":")" +
            error_msg + R"("})");
}

// DataChannelObserver callbacks are handled by DataChannelManager

// ==================== Private Helpers ====================

bool WebRTCEngine::InitializePeerConnection() {
  RTC_DCHECK(!factory_);
  RTC_DCHECK(!peer_connection_);

  if (!network_thread_) {
    network_thread_ = webrtc::Thread::CreateWithSocketServer();
    network_thread_->SetName("app_pc_network_thread", nullptr);
    if (!network_thread_->Start()) {
      RTC_LOG(LS_ERROR) << "Failed to start network thread";
      return false;
    }
  }

  if (!worker_thread_) {
    worker_thread_ = webrtc::Thread::Create();
    worker_thread_->SetName("app_pc_worker_thread", nullptr);
    if (!worker_thread_->Start()) {
      RTC_LOG(LS_ERROR) << "Failed to start worker thread";
      return false;
    }
  }

  if (!signaling_thread_) {
    signaling_thread_ = webrtc::Thread::Create();
    signaling_thread_->SetName("app_pc_signaling_thread", nullptr);
    if (!signaling_thread_->Start()) {
      RTC_LOG(LS_ERROR) << "Failed to start signaling thread";
      return false;
    }
  }

  if (!pipeline_) {
    pipeline_ = std::make_unique<MediaPipeline>(env_, worker_thread_.get());
  }
  if (!pipeline_ || !pipeline_->adm()) {
    if (!pipeline_->CreateAudioDeviceModule()) {
      return false;
    }
  }

  auto pc = PcFactory::Create(network_thread_.get(), worker_thread_.get(),
                               signaling_thread_.get(), env_,
                               pipeline_->adm(), this);
  if (!pc.factory || !pc.connection) {
    RTC_LOG(LS_ERROR) << "Failed to create PeerConnection";
    DeletePeerConnection();
    return false;
  }
  factory_ = std::move(pc.factory);
  peer_connection_ = std::move(pc.connection);
  connection_active_.store(true, std::memory_order_release);

  AddTracks();

  dc_manager_ = std::make_unique<DataChannelManager>();
  dc_manager_->SetEventCallback([this](const std::string& json) {
    if (observer_) observer_->OnEngineEvent(json);
  });
  AddDataChannel();

  return peer_connection_ != nullptr;
}

void WebRTCEngine::DeletePeerConnection() {
  connection_active_.store(false, std::memory_order_release);
  // Clear pending messages first to prevent stale ICE candidates from being sent
  while (!pending_messages_.empty()) {
    delete pending_messages_.front();
    pending_messages_.pop_front();
  }
  pipeline_->StopLocalRenderer();
  pipeline_->StopRemoteRenderer();
  pipeline_->StopRemoteAudioRenderer();
  dc_manager_->Shutdown();
  local_audio_source_ = nullptr;
  peer_connection_->Close();
  peer_connection_ = nullptr;
  factory_ = nullptr;
  local_video_source_ = nullptr;
  peer_id_ = -1;
  loopback_ = false;
}

void WebRTCEngine::AddTracks() {
  if (!peer_connection_->GetSenders().empty()) {
    return;  // Already added tracks.
  }

  // Audio track: select source based on --audio-source flag
  std::string audio_source = absl::GetFlag(FLAGS_audio_source);
  if (audio_source == "shm") {
    local_audio_source_ = ShmAudioCapturer::Create(
        shm_audio_cap_key_path(), SHM_AUDIO_CAP_PROJ_ID);
    if (!local_audio_source_) {
      RTC_LOG(LS_ERROR) << "Failed to create ShmAudioCapturer, falling back to ADM";
      local_audio_source_ =
          factory_->CreateAudioSource(webrtc::AudioOptions());
    }
  } else {
    local_audio_source_ =
        factory_->CreateAudioSource(webrtc::AudioOptions());
  }

  webrtc::scoped_refptr<webrtc::AudioTrackInterface> audio_track(
      factory_->CreateAudioTrack(kAudioLabel, local_audio_source_.get()));
  auto result_or_error = peer_connection_->AddTrack(audio_track, {kStreamId});
  if (!result_or_error.ok()) {
    RTC_LOG(LS_ERROR) << "Failed to add audio track to PeerConnection: "
                      << result_or_error.error().message();
  }

  local_video_source_ = CapturerTrackSource::Create(env_.task_queue_factory());
  if (local_video_source_) {
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> video_track_(
        factory_->CreateVideoTrack(local_video_source_, kVideoLabel));
    pipeline_->StartLocalRenderer(video_track_.get());

    result_or_error = peer_connection_->AddTrack(video_track_, {kStreamId});
    if (!result_or_error.ok()) {
      RTC_LOG(LS_ERROR) << "Failed to add video track to PeerConnection: "
                        << result_or_error.error().message();
    }
  } else {
    RTC_LOG(LS_WARNING)
        << "No local video track; proceeding without local video";
  }

  if (observer_) observer_->OnEngineEvent(R"({"event":"call_connected"})");
}

void WebRTCEngine::AddDataChannel() {
  dc_manager_->Add(peer_connection_.get());
}

void WebRTCEngine::SendMessage(const std::string& json_object) {
  std::string* msg = new std::string(json_object);
  pending_messages_.push_back(msg);

  // If no message is currently being sent, pop and send the front now.
  // Otherwise, the pending message will be picked up by OnMessageSent when
  // the current send completes.
  if (!signaling_->IsSendingMessage()) {
    msg = pending_messages_.front();
    pending_messages_.pop_front();
    if (!signaling_->SendToPeer(peer_id_, *msg) && peer_id_ != -1) {
      RTC_LOG(LS_ERROR) << "SendToPeer failed";
      DisconnectFromServer();
    }
    delete msg;
  }
}

#pragma GCC diagnostic pop
