/*
 *  Copyright 2026 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "apps/peerconnection/client_arm64/webrtc_engine.h"
#include "apps/peerconnection/client_arm64/json_helpers.h"
#include "apps/peerconnection/client_arm64/pc_factory.h"
#include "apps/peerconnection/client_arm64/peer_connection_client.h"

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
#include <set>
#include <sstream>
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
#include "apps/peerconnection/client_arm64/defaults.h"
#include "apps/peerconnection/client_arm64/shm_audio_writer.h"
#include "apps/peerconnection/client_arm64/shm_video_writer.h"
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
  pipeline_->SetAudioMuted(muted);
}

void WebRTCEngine::SetVideoPaused(bool paused) {
  if (signaling_thread_->IsCurrent()) {
    SetVideoPausedImpl(paused);
  } else {
    signaling_thread_->PostTask([this, paused] { SetVideoPausedImpl(paused); });
  }
}

void WebRTCEngine::SetVideoPausedImpl(bool paused) {
  pipeline_->SetVideoPaused(paused, peer_connection_.get());
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
  pipeline_->QueryDevices();
}

void WebRTCEngine::SetVideoDevice(int device_idx) {
  if (signaling_thread_->IsCurrent()) {
    SetVideoDeviceImpl(device_idx);
  } else {
    signaling_thread_->PostTask([this, device_idx] { SetVideoDeviceImpl(device_idx); });
  }
}

void WebRTCEngine::SetVideoDeviceImpl(int device_idx) {
  pipeline_->SetVideoDevice(device_idx);
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

void WebRTCEngine::GetLocalSdp() {
  if (signaling_thread_->IsCurrent()) {
    GetLocalSdpImpl();
  } else {
    signaling_thread_->PostTask([this] { GetLocalSdpImpl(); });
  }
}

void WebRTCEngine::GetLocalSdpImpl() {
  if (peer_connection_) {
    RTC_LOG(LS_ERROR) << "GetLocalSdp: already in a call, hang up first";
    if (observer_)
      observer_->OnEngineEvent(
          R"({"event":"local_sdp_error","error":"Already in a call, hang up first"})");
    return;
  }
  collecting_sdp_ = true;
  if (!InitializePeerConnection()) {
    collecting_sdp_ = false;
    if (observer_)
      observer_->OnEngineEvent(
          R"({"event":"local_sdp_error","error":"Failed to initialize PeerConnection"})");
    return;
  }
  peer_connection_->CreateOffer(
      this, webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
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

// ── SDP filter: keep only H264 in video m-line, force MPP hardware codec ──
static std::string FilterSdpH264Only(const std::string& sdp) {
  std::istringstream in(sdp);
  std::string line;
  std::vector<std::string> lines;
  std::set<int> h264_pts;
  int video_mline_idx = -1;

  // Pass 1: collect lines, find H264 payload types
  while (std::getline(in, line)) {
    lines.push_back(line);
    // a=rtpmap:<pt> H264/...
    if (line.find("a=rtpmap:") == 0 &&
        line.find("H264") != std::string::npos) {
      int pt = 0;
      if (sscanf(line.c_str(), "a=rtpmap:%d", &pt) == 1)
        h264_pts.insert(pt);
    }
    // m=video line
    if (line.find("m=video") == 0)
      video_mline_idx = (int)lines.size() - 1;
  }

  if (video_mline_idx < 0 || h264_pts.empty()) return sdp;

  // Pass 2: rewrite video m-line — keep only H264 payload types
  // Format: m=video 9 UDP/TLS/RTP/SAVPF 96 97 98 99 ...
  std::string& mline = lines[video_mline_idx];
  size_t space_after_proto = mline.rfind("SAVPF");
  if (space_after_proto == std::string::npos)
    space_after_proto = mline.rfind("SAVP ");  // non-bundle fallback
  if (space_after_proto == std::string::npos) return sdp;

  std::string prefix = mline.substr(0, mline.find(' ', space_after_proto));
  std::string new_mline = prefix;
  std::set<int> kept_pts;
  for (int pt : h264_pts) {
    new_mline += " " + std::to_string(pt);
    kept_pts.insert(pt);
  }
  // Also keep RTX and FLEXFEC retransmission payloads associated with H264
  for (size_t i = 0; i < lines.size(); i++) {
    if (lines[i].find("a=rtpmap:") != 0) continue;
    int pt = 0;
    sscanf(lines[i].c_str(), "a=rtpmap:%d", &pt);
    if (kept_pts.count(pt)) continue;
    if (lines[i].find("rtx") != std::string::npos ||
        lines[i].find("flexfec") != std::string::npos) {
      // Check if this RTX/FLEXFEC is associated with a kept H264 pt
      // via a=fmtp:<rtx_pt> apt=<h264_pt>
      for (const auto& check_line : lines) {
        if (check_line.find("a=fmtp:" + std::to_string(pt)) == 0) {
          for (int hpt : kept_pts) {
            if (check_line.find("apt=" + std::to_string(hpt)) != std::string::npos) {
              new_mline += " " + std::to_string(pt);
              kept_pts.insert(pt);
              break;
            }
          }
        }
      }
    }
  }
  lines[video_mline_idx] = new_mline;

  // Filter non-kept rtpmap/fmtp lines in video section
  bool in_video = false;
  for (size_t i = 0; i < lines.size(); i++) {
    if (lines[i].find("m=video") == 0) {
      in_video = true; continue;
    }
    if (i > (size_t)video_mline_idx && lines[i].find("m=") == 0) {
      in_video = false; continue;
    }
    if (!in_video) continue;
    if (lines[i].find("a=rtpmap:") != 0 && lines[i].find("a=fmtp:") != 0)
      continue;
    int pt = 0;
    sscanf(lines[i].c_str(), lines[i][2] == 'r' ? "a=rtpmap:%d" : "a=fmtp:%d", &pt);
    if (!kept_pts.count(pt)) lines[i] = "";  // mark for removal
  }

  std::string result;
  for (const auto& l : lines) {
    if (!l.empty()) { result += l; result += "\r\n"; }
  }
  return result;
}

// ==================== CreateSessionDescriptionObserver ====================

void WebRTCEngine::OnSuccess(webrtc::SessionDescriptionInterface* desc) {
  std::string sdp;
  desc->ToString(&sdp);

  if (collecting_sdp_) {
    collecting_sdp_ = false;
    Json::Value j;
    j["event"] = "local_sdp";
    j["sdp"] = sdp;
    Json::StreamWriterBuilder factory;
    factory["indentation"] = "";
    if (observer_) observer_->OnEngineEvent(Json::writeString(factory, j));
    signaling_thread_->PostTask([this] { DeletePeerConnection(); });
    return;
  }

  peer_connection_->SetLocalDescription(
      DummySetSessionDescriptionObserver::Create().get(), desc);

  sdp = FilterSdpH264Only(sdp);

  Json::Value jmessage;
  jmessage[kSessionDescriptionTypeName] =
      webrtc::SdpTypeToString(desc->GetType());
  jmessage[kSessionDescriptionSdpName] = sdp;

  Json::StreamWriterBuilder factory;
  SendMessage(Json::writeString(factory, jmessage));
}

void WebRTCEngine::OnFailure(webrtc::RTCError error) {
  RTC_LOG(LS_ERROR) << ToString(error.type()) << ": " << error.message();
  if (collecting_sdp_) {
    collecting_sdp_ = false;
    if (observer_)
      observer_->OnEngineEvent(
          R"({"event":"local_sdp_error","error":"CreateOffer failed"})");
    signaling_thread_->PostTask([this] { DeletePeerConnection(); });
  }
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
    auto adm = pipeline_->adm();
    pipeline_->Shutdown();
    dc_manager_->Shutdown();
    peer_id_ = -1;
    loopback_ = false;
    signaling_->Close();
    signaling_thread_->PostTask([this, pc = std::move(pc), f = std::move(f),
                                  adm = std::move(adm)]() mutable {
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
    pipeline_->SetEventCallback([this](const std::string& json) {
      if (observer_) observer_->OnEngineEvent(json);
    });
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
  pipeline_->Shutdown();
  dc_manager_->Shutdown();
  peer_connection_->Close();
  peer_connection_ = nullptr;
  factory_ = nullptr;
  peer_id_ = -1;
  loopback_ = false;
}

void WebRTCEngine::AddTracks() {
  if (!peer_connection_->GetSenders().empty()) {
    return;  // Already added tracks.
  }

  // Audio track: create SHM source if requested, else ADM via pipeline
  webrtc::scoped_refptr<webrtc::AudioSourceInterface> shm_source;
  std::string audio_source = absl::GetFlag(FLAGS_audio_source);
  if (audio_source == "shm") {
    shm_source = ShmAudioCapturer::Create(
        shm_audio_cap_key_path(), SHM_AUDIO_CAP_PROJ_ID);
    if (!shm_source)
      RTC_LOG(LS_ERROR) << "Failed to create ShmAudioCapturer, falling back to ADM";
  }

  auto audio_src = pipeline_->CreateAudioSource(factory_.get(), shm_source.get());
  auto audio_track = factory_->CreateAudioTrack(kAudioLabel, audio_src);
  auto result_or_error = peer_connection_->AddTrack(audio_track, {kStreamId});
  if (!result_or_error.ok()) {
    RTC_LOG(LS_ERROR) << "Failed to add audio track to PeerConnection: "
                      << result_or_error.error().message();
  }

  auto video_source = pipeline_->CreateVideoSource();
  if (video_source) {
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> video_track_(
        factory_->CreateVideoTrack(video_source, kVideoLabel));
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
