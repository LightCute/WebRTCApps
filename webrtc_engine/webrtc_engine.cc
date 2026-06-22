/*
 *  Copyright 2026 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "apps/webrtc_engine/webrtc_engine.h"
#include "apps/webrtc_engine/json_helpers.h"
#include "apps/webrtc_engine/pc_factory_interface.h"
#include "apps/webrtc_engine/peer_connection_client.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunsafe-buffer-usage"
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"

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
#include "apps/webrtc_engine/defaults.h"
#include "apps/peerconnection/client/shm_video_writer.h"
#include "common_video/libyuv/include/webrtc_libyuv.h"
#include "json/json.h"
#include "modules/video_capture/video_capture.h"
#include "modules/video_capture/video_capture_factory.h"
#include "pc/video_track_source.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"
#include "rtc_base/strings/json.h"
#include "rtc_base/thread.h"
#include <map>
#include "api/stats/rtc_stats.h"
#include "api/stats/rtc_stats_collector_callback.h"
#include "api/stats/rtc_stats_report.h"
#include "api/stats/rtcstats_objects.h"
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
  // Default: create PeerConnectionClient if not injected via SetSignaling
  if (!signaling_) {
    signaling_ = std::make_unique<PeerConnectionClient>();
  }
  signaling_->RegisterObserver(this);
}

// ---- Dependency injection ----

void WebRTCEngine::SetMediaPipeline(std::unique_ptr<IMediaPipeline> pipeline) {
  pipeline_ = std::move(pipeline);
}
void WebRTCEngine::SetPcFactory(std::unique_ptr<IPcFactory> factory) {
  pc_factory_injected_ = std::move(factory);
}
void WebRTCEngine::SetIpcServer(std::unique_ptr<IIpcServer> server) {
  ipc_server_ = std::move(server);
}
void WebRTCEngine::SetSignaling(std::unique_ptr<SignalingInterface> signaling) {
  signaling_ = std::move(signaling);
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
  // OnSuccess will emit the SDP and clean up
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
  }
}

void WebRTCEngine::OnRemoveTrack(
    webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) {
  RTC_LOG(LS_INFO) << __FUNCTION__ << " " << receiver->id();

  auto* track = receiver->track().get();
  if (track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
    pipeline_->StopRemoteRenderer();
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
    // Clean up — post to avoid re-entrancy in CreateOffer callback
    signaling_thread_->PostTask([this] { DeletePeerConnection(); });
    return;
  }

  peer_connection_->SetLocalDescription(
      DummySetSessionDescriptionObserver::Create().get(), desc);

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
    RTC_LOG(LS_ERROR) << "MediaPipeline not injected. Call SetMediaPipeline() first.";
    return false;
  }
  pipeline_->SetEventCallback([this](const std::string& json) {
    if (observer_) observer_->OnEngineEvent(json);
  });
  if (!pipeline_->adm()) {
    if (!pipeline_->CreateAudioDeviceModule()) {
      RTC_LOG(LS_ERROR) << "Failed to create AudioDeviceModule";
      return false;
    }
  }

  if (!pc_factory_injected_) {
    RTC_LOG(LS_ERROR) << "PcFactory not injected. Call SetPcFactory() first.";
    return false;
  }
  PcComponents pc = pc_factory_injected_->Create(
      network_thread_.get(), worker_thread_.get(), signaling_thread_.get(),
      env_, pipeline_->adm(), this);
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

  // Audio track
  auto audio_src = pipeline_->CreateAudioSource(factory_.get());
  auto audio_track = factory_->CreateAudioTrack(kAudioLabel, audio_src);
  auto audio_result = peer_connection_->AddTrack(audio_track, {kStreamId});
  if (!audio_result.ok()) {
    RTC_LOG(LS_ERROR) << "Failed to add audio track to PeerConnection: "
                      << audio_result.error().message();
  }

  auto video_source = pipeline_->CreateVideoSource();
  if (video_source) {
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> video_track_(
        factory_->CreateVideoTrack(video_source, kVideoLabel));
    pipeline_->StartLocalRenderer(video_track_.get());

    auto result_or_error = peer_connection_->AddTrack(video_track_, {kStreamId});
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

// ==================== Stats Reporting ====================

namespace {
class StatsCallback : public webrtc::RTCStatsCollectorCallback {
 public:
  explicit StatsCallback(EngineObserver* obs) : observer_(obs) {}
  void OnStatsDelivered(
      const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report) override {
    if (!observer_) return;
    Json::Value json;
    json["event"] = "stats";

    // --- First pass: build candidate ID → info lookup ---
    std::map<std::string, std::string> cand_type;       // id → type (host/srflx/relay)
    std::map<std::string, std::string> cand_addr;       // id → "ip:port"
    std::map<std::string, std::string> cand_proto;      // id → udp/tcp
    for (const auto& stat : *report) {
      auto type = std::string(stat.type());
      if (type == "local-candidate") {
        auto& s = stat.cast_to<webrtc::RTCLocalIceCandidateStats>();
        if (s.candidate_type.has_value())
          cand_type[stat.id()] = *s.candidate_type;
        std::string ip = s.ip.value_or("");
        if (ip.empty() && s.address.has_value()) ip = *s.address;
        if (ip.empty() && s.related_address.has_value()) ip = *s.related_address;
        if (!ip.empty()) {
          std::string addr = ip;
          if (s.port.has_value())
            addr += ":" + std::to_string(*s.port);
          cand_addr[stat.id()] = addr;
        }
        if (s.protocol.has_value())
          cand_proto[stat.id()] = *s.protocol;
      } else if (type == "remote-candidate") {
        auto& s = stat.cast_to<webrtc::RTCRemoteIceCandidateStats>();
        if (s.candidate_type.has_value())
          cand_type[stat.id()] = *s.candidate_type;
        std::string ip = s.ip.value_or("");
        if (ip.empty() && s.address.has_value()) ip = *s.address;
        if (!ip.empty() && s.port.has_value())
          cand_addr[stat.id()] = ip + ":" + std::to_string(*s.port);
        if (s.protocol.has_value())
          cand_proto[stat.id()] = *s.protocol;
      }
    }

    // --- Second pass: collect stream + transport + codec stats ---
    for (const auto& stat : *report) {
      auto type = std::string(stat.type());

      // === Video: outbound-rtp (what you're sending) ===
      if (type == "outbound-rtp") {
        auto& s = stat.cast_to<webrtc::RTCOutboundRtpStreamStats>();
        if (!s.kind.has_value() || *s.kind != "video") continue;
        json["encode_fps"]    = static_cast<int>(s.frames_per_second.value_or(0));
        json["encode_w"]      = static_cast<int>(s.frame_width.value_or(0));
        json["encode_h"]      = static_cast<int>(s.frame_height.value_or(0));
        json["frames_enc"]    = static_cast<int>(s.frames_encoded.value_or(0));
        json["key_frames_enc"] = static_cast<int>(s.key_frames_encoded.value_or(0));
        json["nack_sent"]     = static_cast<int>(s.nack_count.value_or(0));
        json["pli_sent"]      = static_cast<int>(s.pli_count.value_or(0));
        json["fir_sent"]      = static_cast<int>(s.fir_count.value_or(0));
        json["target_kbps"]   = static_cast<int>(s.target_bitrate.value_or(0) / 1000);
        json["bytes_sent"]    = static_cast<int64_t>(s.bytes_sent.value_or(0));
        json["pkt_sent"]      = static_cast<int64_t>(s.packets_sent.value_or(0));
        json["retx_pkt_sent"] = static_cast<int>(s.retransmitted_packets_sent.value_or(0));
        if (s.content_type.has_value())
          json["content_type"] = std::string(*s.content_type);
        if (s.quality_limitation_reason.has_value())
          json["quality_limit"] = std::string(*s.quality_limitation_reason);
        if (s.quality_limitation_durations.has_value()) {
          auto& d = *s.quality_limitation_durations;
          json["limit_none_s"] = d.contains("none") ? d.at("none") : 0;
          json["limit_cpu_s"]  = d.contains("cpu") ? d.at("cpu") : 0;
          json["limit_bw_s"]   = d.contains("bandwidth") ? d.at("bandwidth") : 0;
        }
        if (s.total_encode_time.has_value() && s.frames_encoded.value_or(1) > 0)
          json["avg_encode_ms"] = static_cast<int>(
              *s.total_encode_time / *s.frames_encoded * 1000);
        if (s.encoder_implementation.has_value())
          json["encoder"] = std::string(*s.encoder_implementation);
      }

      // === Video: inbound-rtp (what you're receiving) ===
      if (type == "inbound-rtp") {
        auto& s = stat.cast_to<webrtc::RTCInboundRtpStreamStats>();
        if (!s.kind.has_value() || *s.kind != "video") continue;
        json["decode_fps"]    = static_cast<int>(s.frames_per_second.value_or(0));
        json["decode_w"]      = static_cast<int>(s.frame_width.value_or(0));
        json["decode_h"]      = static_cast<int>(s.frame_height.value_or(0));
        json["frames_dec"]    = static_cast<int>(s.frames_decoded.value_or(0));
        json["key_frames_dec"] = static_cast<int>(s.key_frames_decoded.value_or(0));
        json["pkt_lost"]      = static_cast<int>(s.packets_lost.value_or(0));
        json["pkt_recv"]      = static_cast<int>(s.packets_received.value_or(0));
        int64_t total_pkts = static_cast<int64_t>(s.packets_lost.value_or(0))
                           + static_cast<int64_t>(s.packets_received.value_or(0));
        if (total_pkts > 0)
          json["loss_rate_pct"] = static_cast<int>(
              s.packets_lost.value_or(0) * 10000 / total_pkts) / 100.0;
        else
          json["loss_rate_pct"] = 0.0;
        json["bytes_recv"]    = static_cast<int64_t>(s.bytes_received.value_or(0));
        json["jitter_s"]      = s.jitter.value_or(0);
        json["nack_recv"]     = static_cast<int>(s.nack_count.value_or(0));
        json["pli_recv"]      = static_cast<int>(s.pli_count.value_or(0));
        json["fir_recv"]      = static_cast<int>(s.fir_count.value_or(0));
        if (s.total_decode_time.has_value() && s.frames_decoded.value_or(1) > 0)
          json["avg_decode_ms"] = static_cast<int>(
              *s.total_decode_time / *s.frames_decoded * 1000);
        if (s.decoder_implementation.has_value())
          json["decoder"] = std::string(*s.decoder_implementation);
        // Freeze detection
        if (s.freeze_count.has_value())
          json["freeze_cnt"] = static_cast<int>(*s.freeze_count);
        // JitterBuffer discards
        if (s.packets_discarded.has_value())
          json["pkt_discarded"] = static_cast<int>(*s.packets_discarded);
      }

      // === Audio: outbound-rtp (mic → network) ===
      if (type == "outbound-rtp") {
        auto& s = stat.cast_to<webrtc::RTCOutboundRtpStreamStats>();
        if (!s.kind.has_value() || *s.kind != "audio") continue;
        json["audio_sent_kbps"] = static_cast<int>(s.target_bitrate.value_or(0) / 1000);
        json["audio_enc_pkt"]   = static_cast<int>(s.packets_sent.value_or(0));
      }

      // === Audio: inbound-rtp (network → speaker) ===
      if (type == "inbound-rtp") {
        auto& s = stat.cast_to<webrtc::RTCInboundRtpStreamStats>();
        if (!s.kind.has_value() || *s.kind != "audio") continue;
        json["audio_recv_kbps"] = static_cast<int>(
            s.bytes_received.value_or(0) * 8 / 1000);
        json["audio_pkt_lost"]  = static_cast<int>(s.packets_lost.value_or(0));
        json["audio_jitter_s"]  = s.jitter.value_or(0);
      }

      // === ICE candidate pair (the active connection) ===
      if (type == "candidate-pair") {
        auto& s = stat.cast_to<webrtc::RTCIceCandidatePairStats>();
        if (std::string(*s.state) != "succeeded") continue;
        json["rtt_s"]          = s.current_round_trip_time.value_or(0);
        json["avail_kbps"]     = static_cast<int>(s.available_outgoing_bitrate.value_or(0) / 1000);
        json["ice_nominated"]  = s.nominated.value_or(false);
        json["ice_writable"]   = s.writable.value_or(false);
        json["pair_pkt_sent"]  = static_cast<int64_t>(s.packets_sent.value_or(0));
        json["pair_pkt_recv"]  = static_cast<int>(s.packets_received.value_or(0));
        // Resolve candidate names
        if (s.local_candidate_id.has_value()) {
          auto local_id = *s.local_candidate_id;
          json["local_cand_type"] = cand_type.count(local_id) ? cand_type[local_id] : "?";
          json["local_cand_addr"] = cand_addr.count(local_id) ? cand_addr[local_id] : "?";
        }
        if (s.remote_candidate_id.has_value()) {
          auto remote_id = *s.remote_candidate_id;
          json["remote_cand_type"] = cand_type.count(remote_id) ? cand_type[remote_id] : "?";
          json["remote_cand_addr"] = cand_addr.count(remote_id) ? cand_addr[remote_id] : "?";
          // prflx candidates may have address only in the transport-level info,
          // not in the RTCStats. Log the raw candidate ID for debugging.
          if (cand_type.count(remote_id) && cand_addr.count(remote_id)) {
            json["remote_cand_addr"] = cand_addr[remote_id];
          } else {
            // prflx case: the WebRTC stats API doesn't expose IP for peer-reflexive
            // candidates. The address can be found in ICE transport-level logs.
            json["remote_cand_addr"] = "(prflx, see ICE logs)";
          }
        }
        // STUN connectivity check counters
        if (s.requests_sent.has_value())
          json["stun_req_sent"] = static_cast<int>(*s.requests_sent);
        if (s.responses_received.has_value())
          json["stun_resp_recv"] = static_cast<int>(*s.responses_received);
        if (s.consent_requests_sent.has_value())
          json["consent_sent"] = static_cast<int>(*s.consent_requests_sent);
      }

      // === Transport (DTLS) ===
      if (type == "transport") {
        auto& s = stat.cast_to<webrtc::RTCTransportStats>();
        if (s.dtls_state.has_value())
          json["dtls_state"] = std::string(*s.dtls_state);
        json["transport_pkt_sent"] = static_cast<int64_t>(s.packets_sent.value_or(0));
        json["transport_pkt_recv"] = static_cast<int64_t>(s.packets_received.value_or(0));
      }
    }
    // --- Real-time kbps from cumulative bytes (2s polling interval) ---
    static int64_t last_bytes_sent = 0, last_bytes_recv = 0;
    static int64_t last_ts_us = 0;
    int64_t now_bytes_sent = json.get("bytes_sent", Json::Value(0)).asInt64();
    int64_t now_bytes_recv = json.get("bytes_recv", Json::Value(0)).asInt64();
    int64_t now_ts_us = webrtc::TimeMicros();
    if (last_ts_us > 0 && now_ts_us > last_ts_us) {
      double dt = (now_ts_us - last_ts_us) / 1e6;
      json["send_kbps"] = static_cast<int>(
          (now_bytes_sent - last_bytes_sent) * 8 / dt / 1000);
      json["recv_kbps"] = static_cast<int>(
          (now_bytes_recv - last_bytes_recv) * 8 / dt / 1000);
    }
    last_bytes_sent = now_bytes_sent;
    last_bytes_recv = now_bytes_recv;
    last_ts_us = now_ts_us;

    Json::StreamWriterBuilder factory;
    factory["indentation"] = "";
    observer_->OnEngineEvent(Json::writeString(factory, json));
  }

  void AddRef() const override {}
  webrtc::RefCountReleaseStatus Release() const override {
    return webrtc::RefCountReleaseStatus::kOtherRefsRemained;
  }

  EngineObserver* observer_;
};
}  // namespace

void WebRTCEngine::DumpStats() {
  if (!peer_connection_) return;
  webrtc::scoped_refptr<StatsCallback> cb(new StatsCallback(observer_));
  peer_connection_->GetStats(cb.get());
}

void WebRTCEngine::StartStatsPolling() {
  if (stats_polling_) return;
  signaling_thread_->PostTask([this] {
    if (stats_polling_) return;
    stats_polling_ = webrtc::RepeatingTaskHandle::Start(
        signaling_thread_.get(),
        [this] {
          DumpStats();
          return webrtc::TimeDelta::Seconds(2);
        });
    RTC_LOG(LS_INFO) << "Stats polling started (2s interval)";
  });
}

void WebRTCEngine::StopStatsPolling() {
  if (!stats_polling_) return;
  // Stop must happen on the same task queue that runs the timer (signaling thread).
  signaling_thread_->PostTask([this] {
    if (stats_polling_) {
      stats_polling_->Stop();
      stats_polling_.reset();
    }
  });
  RTC_LOG(LS_INFO) << "Stats polling stopped";
}

void WebRTCEngine::QueryVideoCaps() {
  if (!pipeline_) return;
  std::string json = pipeline_->GetVideoCapabilities();
  if (observer_) observer_->OnEngineEvent(json);
}

void WebRTCEngine::SetVideoParams(int width, int height, int fps) {
  if (!pipeline_) return;
  pipeline_->SetVideoParams(width, height, fps);
  // Notify back
  if (observer_) {
    Json::Value root;
    root["event"] = "video_params_changed";
    root["width"] = width; root["height"] = height; root["fps"] = fps;
    Json::StreamWriterBuilder f; f["indentation"] = "";
    observer_->OnEngineEvent(Json::writeString(f, root));
  }
}

#pragma GCC diagnostic pop
