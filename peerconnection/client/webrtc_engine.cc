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

// Names used for a IceCandidate JSON object.
const char kCandidateSdpMidName[] = "sdpMid";
const char kCandidateSdpMlineIndexName[] = "sdpMLineIndex";
const char kCandidateSdpName[] = "candidate";

// Names used for a SessionDescription JSON object.
const char kSessionDescriptionTypeName[] = "type";
const char kSessionDescriptionSdpName[] = "sdp";

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

// JSON escaping helper for data channel messages.
std::string EscapeJsonString(const std::string& input) {
  std::string output;
  output.reserve(input.size());
  for (char c : input) {
    switch (c) {
      case '"':  output += "\\\""; break;
      case '\\': output += "\\\\"; break;
      case '\b': output += "\\b";  break;
      case '\f': output += "\\f";  break;
      case '\n': output += "\\n";  break;
      case '\r': output += "\\r";  break;
      case '\t': output += "\\t";  break;
      default:   output += c;      break;
    }
  }
  return output;
}

// Build a JSON peer list event string from the peers map.
std::string BuildPeerListJson(const Peers& peers) {
  std::string json = R"({"event":"peer_list","peers":[)";
  bool first = true;
  for (const auto& p : peers) {
    if (!first)
      json += ",";
    first = false;
    json += "{\"id\":" + std::to_string(p.first) +
            ",\"name\":\"" + p.second + "\"}";
  }
  json += "]}";
  return json;
}

// Convert ICE connection state to string for JSON events.
const char* IceConnectionStateToString(
    webrtc::PeerConnectionInterface::IceConnectionState state) {
  switch (state) {
    case webrtc::PeerConnectionInterface::kIceConnectionNew:
      return "new";
    case webrtc::PeerConnectionInterface::kIceConnectionChecking:
      return "checking";
    case webrtc::PeerConnectionInterface::kIceConnectionConnected:
      return "connected";
    case webrtc::PeerConnectionInterface::kIceConnectionCompleted:
      return "completed";
    case webrtc::PeerConnectionInterface::kIceConnectionFailed:
      return "failed";
    case webrtc::PeerConnectionInterface::kIceConnectionDisconnected:
      return "disconnected";
    case webrtc::PeerConnectionInterface::kIceConnectionClosed:
      return "closed";
    default:
      return "unknown";
  }
}

// Convert DataChannel state to string.
const char* DataChannelStateToString(webrtc::DataChannelInterface::DataState s) {
  switch (s) {
    case webrtc::DataChannelInterface::kConnecting: return "connecting";
    case webrtc::DataChannelInterface::kOpen:        return "open";
    case webrtc::DataChannelInterface::kClosing:     return "closing";
    case webrtc::DataChannelInterface::kClosed:      return "closed";
    default:                                         return "unknown";
  }
}

}  // namespace

// ==================== ShmVideoSink ====================

WebRTCEngine::ShmVideoSink::ShmVideoSink(const std::string& key_path,
                                          int proj_id)
    : writer_(std::make_unique<ShmVideoWriter>()),
      io_thread_(&ShmVideoSink::IoLoop, this) {
  if (!writer_->Init(key_path, proj_id)) {
    RTC_LOG(LS_ERROR) << "ShmVideoSink: ShmVideoWriter::Init failed for "
                      << key_path;
  }
}

WebRTCEngine::ShmVideoSink::~ShmVideoSink() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
  }
  cv_.notify_all();
  if (io_thread_.joinable()) {
    io_thread_.join();
  }
}

void WebRTCEngine::ShmVideoSink::OnFrame(const webrtc::VideoFrame& frame) {
  webrtc::scoped_refptr<webrtc::I420BufferInterface> i420 =
      frame.video_frame_buffer()->ToI420();
  if (!i420) {
    RTC_LOG(LS_WARNING) << "ShmVideoSink::OnFrame: ToI420 returned null";
    return;
  }

  int width = i420->width();
  int height = i420->height();
  int half_width = (width + 1) / 2;
  int y_size = i420->StrideY() * height;
  int u_size = i420->StrideU() * ((height + 1) / 2);
  int v_size = i420->StrideV() * ((height + 1) / 2);
  int total_size = y_size + u_size + v_size;

  FrameData fd;
  fd.head.ntp_time_ms = frame.ntp_time_ms();
  fd.head.width = static_cast<uint16_t>(width);
  fd.head.height = static_cast<uint16_t>(height);
  fd.head.frame_type = 0;
  fd.head.rotation = frame.rotation();
  fd.head.frame_len = static_cast<uint32_t>(total_size);
  fd.i420_data.resize(total_size);

  // Copy Y plane (respecting stride)
  const uint8_t* src_y = i420->DataY();
  uint8_t* dst = fd.i420_data.data();
  for (int row = 0; row < height; ++row) {
    std::memcpy(dst, src_y, width);
    dst += width;
    src_y += i420->StrideY();
  }
  // Copy U plane (respecting stride)
  const uint8_t* src_u = i420->DataU();
  for (int row = 0; row < (height + 1) / 2; ++row) {
    std::memcpy(dst, src_u, half_width);
    dst += half_width;
    src_u += i420->StrideU();
  }
  // Copy V plane (respecting stride)
  const uint8_t* src_v = i420->DataV();
  for (int row = 0; row < (height + 1) / 2; ++row) {
    std::memcpy(dst, src_v, half_width);
    dst += half_width;
    src_v += i420->StrideV();
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_ = std::move(fd);
  }
  cv_.notify_one();
}

void WebRTCEngine::ShmVideoSink::IoLoop() {
  while (true) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return pending_.has_value() || stopped_; });
    if (stopped_) {
      return;
    }
    if (pending_.has_value()) {
      FrameData fd = std::move(*pending_);
      pending_.reset();
      lock.unlock();
      writer_->WriteFrame(fd.head, fd.i420_data.data());
    }
  }
}

// ==================== ShmAudioSink ====================

WebRTCEngine::ShmAudioSink::ShmAudioSink(const std::string& key_path,
                                          int proj_id)
    : writer_(std::make_unique<ShmAudioWriter>()),
      io_thread_(&ShmAudioSink::IoLoop, this) {
  if (!writer_->Init(key_path, proj_id)) {
    RTC_LOG(LS_ERROR) << "ShmAudioSink: ShmAudioWriter::Init failed for "
                      << key_path;
  }
}

WebRTCEngine::ShmAudioSink::~ShmAudioSink() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
  }
  cv_.notify_all();
  if (io_thread_.joinable()) {
    io_thread_.join();
  }
}

void WebRTCEngine::ShmAudioSink::OnData(
    const void* audio_data, int bits_per_sample, int sample_rate,
    size_t number_of_channels, size_t number_of_frames,
    std::optional<int64_t> absolute_capture_timestamp_ms) {
  size_t byte_count =
      number_of_frames * number_of_channels * (bits_per_sample / 8);
  if (byte_count > AUDIO_FRAME_MAX_SIZE) {
    RTC_LOG(LS_WARNING) << "ShmAudioSink: frame too large: " << byte_count;
    return;
  }

  PendingAudio pending;
  pending.head.ntp_time_ms = absolute_capture_timestamp_ms.value_or(0);
  pending.head.frame_len = static_cast<uint32_t>(byte_count);
  pending.head.sample_rate = static_cast<uint32_t>(sample_rate);
  pending.head.channels = static_cast<uint16_t>(number_of_channels);
  pending.head.bits_per_sample = static_cast<uint16_t>(bits_per_sample);
  pending.data.assign(static_cast<const uint8_t*>(audio_data),
                      static_cast<const uint8_t*>(audio_data) + byte_count);

  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_ = std::move(pending);
  }
  cv_.notify_one();
}

void WebRTCEngine::ShmAudioSink::IoLoop() {
  while (true) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return pending_.has_value() || stopped_; });
    if (stopped_) {
      return;
    }
    if (pending_.has_value()) {
      auto pending = std::move(*pending_);
      pending_.reset();
      lock.unlock();
      writer_->WriteFrame(pending.head, pending.data.data());
    }
  }
}

// ==================== ShmAudioSource ====================

webrtc::scoped_refptr<WebRTCEngine::ShmAudioSource>
WebRTCEngine::ShmAudioSource::Create(const std::string& key_path, int proj_id) {
  auto source = webrtc::make_ref_counted<ShmAudioSource>();
  if (!source->reader_.Init(key_path, proj_id)) {
    RTC_LOG(LS_ERROR) << "ShmAudioSource: ShmAudioReader::Init failed for "
                      << key_path;
    return nullptr;
  }
  source->running_ = true;
  source->capture_thread_ =
      std::thread(&ShmAudioSource::CaptureLoop, source.get());
  return source;
}

WebRTCEngine::ShmAudioSource::~ShmAudioSource() {
  running_ = false;
  reader_.RequestStop();
  if (capture_thread_.joinable()) {
    capture_thread_.join();
  }
}

void WebRTCEngine::ShmAudioSource::AddSink(
    webrtc::AudioTrackSinkInterface* sink) {
  std::lock_guard<std::mutex> lock(sinks_mutex_);
  sinks_.push_back(sink);
}

void WebRTCEngine::ShmAudioSource::RemoveSink(
    webrtc::AudioTrackSinkInterface* sink) {
  std::lock_guard<std::mutex> lock(sinks_mutex_);
  sinks_.erase(std::remove(sinks_.begin(), sinks_.end(), sink), sinks_.end());
}

void WebRTCEngine::ShmAudioSource::CaptureLoop() {
  std::vector<uint8_t> buf(AUDIO_FRAME_MAX_SIZE);
  while (running_) {
    AudioFrameHead head;
    if (!reader_.ReadFrame(head, buf.data(), AUDIO_FRAME_MAX_SIZE)) {
      break;
    }

    std::lock_guard<std::mutex> lock(sinks_mutex_);
    for (auto* sink : sinks_) {
      if (sink) {
        sink->OnData(buf.data(), head.bits_per_sample,
                     static_cast<int>(head.sample_rate), head.channels,
                     head.frame_len /
                         (head.channels * (head.bits_per_sample / 8)),
                     head.ntp_time_ms);
      }
    }
  }
}

// ==================== WebRTCEngine ====================

WebRTCEngine::WebRTCEngine(const webrtc::Environment& env)
    : env_(env),
      safety_(webrtc::PendingTaskSafetyFlag::Create()),
      peer_id_(-1),
      loopback_(false) {
  signaling_client_.RegisterObserver(this);
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

  if (media_)
    media_->Shutdown();

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
  signaling_client_.SignOut();
  DeletePeerConnection();

  // Clean up pending messages.
  while (!pending_messages_.empty()) {
    delete pending_messages_.front();
    pending_messages_.pop_front();
  }

  // Stop SHM renderers.
  StopLocalShmRenderer();
  StopRemoteShmRenderer();
  StopRemoteAudioShmRenderer();

  if (media_)
    media_->Shutdown();

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
  if (signaling_client_.is_connected())
    return;
  server_ = server;
  server_port_ = port;
  signaling_client_.Connect(server, port, GetPeerName());
}

void WebRTCEngine::DisconnectFromServer() {
  if (signaling_thread_->IsCurrent()) {
    DisconnectFromServerImpl();
  } else {
    signaling_thread_->PostTask([this] { DisconnectFromServerImpl(); });
  }
}

void WebRTCEngine::DisconnectFromServerImpl() {
  if (signaling_client_.is_connected())
    signaling_client_.SignOut();
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
    signaling_client_.SendHangUp(peer_id_);
  }
  //on_event_(BuildPeerListJson(signaling_client_.peers()));
}

void WebRTCEngine::SetAudioMuted(bool muted) {
  if (signaling_thread_->IsCurrent()) {
    SetAudioMutedImpl(muted);
  } else {
    signaling_thread_->PostTask([this, muted] { SetAudioMutedImpl(muted); });
  }
}

void WebRTCEngine::SetAudioMutedImpl(bool muted) {
  media_->SetAudioMuted(muted);
}

void WebRTCEngine::SetVideoPaused(bool paused) {
  if (signaling_thread_->IsCurrent()) {
    SetVideoPausedImpl(paused);
  } else {
    signaling_thread_->PostTask([this, paused] { SetVideoPausedImpl(paused); });
  }
}

void WebRTCEngine::SetVideoPausedImpl(bool paused) {
  media_->SetVideoPaused(paused, peer_connection_.get());
}

void WebRTCEngine::SendData(const std::string& text) {
  if (signaling_thread_->IsCurrent()) {
    SendDataImpl(text);
  } else {
    signaling_thread_->PostTask([this, text] { SendDataImpl(text); });
  }
}

void WebRTCEngine::SendDataImpl(const std::string& text) {
  if (data_channel_ &&
      data_channel_->state() == webrtc::DataChannelInterface::kOpen) {
    data_channel_->Send(webrtc::DataBuffer(text));
  } else {
    RTC_LOG(LS_WARNING) << "SendData: data channel not open";
  }
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
  media_->QueryDevices();
}

void WebRTCEngine::SetVideoDevice(int device_idx) {
  if (signaling_thread_->IsCurrent()) {
    SetVideoDeviceImpl(device_idx);
  } else {
    signaling_thread_->PostTask([this, device_idx] { SetVideoDeviceImpl(device_idx); });
  }
}

void WebRTCEngine::SetVideoDeviceImpl(int device_idx) {
  media_->SetVideoDevice(device_idx);
}

void WebRTCEngine::SetAudioInputDevice(int device_idx) {
  if (signaling_thread_->IsCurrent()) {
    SetAudioInputDeviceImpl(device_idx);
  } else {
    signaling_thread_->PostTask([this, device_idx] { SetAudioInputDeviceImpl(device_idx); });
  }
}

void WebRTCEngine::SetAudioInputDeviceImpl(int device_idx) {
  media_->SetAudioInputDevice(device_idx);
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
    StartRemoteShmRenderer(video_track);
  } else if (track->kind() == webrtc::MediaStreamTrackInterface::kAudioKind) {
    auto* audio_track = static_cast<webrtc::AudioTrackInterface*>(track);
    StartRemoteAudioShmRenderer(audio_track);
  }
}

void WebRTCEngine::OnRemoveTrack(
    webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) {
  RTC_LOG(LS_INFO) << __FUNCTION__ << " " << receiver->id();

  auto* track = receiver->track().get();
  if (track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
    StopRemoteShmRenderer();
  } else if (track->kind() == webrtc::MediaStreamTrackInterface::kAudioKind) {
    StopRemoteAudioShmRenderer();
  }
}

void WebRTCEngine::OnDataChannel(
    webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) {
  if (!channel) {
    RTC_LOG(LS_ERROR) << "OnDataChannel: Received null DataChannel";
    return;
  }
  if (channel->label() != "chat") {
    RTC_LOG(LS_WARNING) << "OnDataChannel: Unexpected label: " << channel->label();
    return;
  }
  if (data_channel_) {
    RTC_LOG(LS_WARNING) << "DataChannel already exists, replacing...";
  }
  data_channel_ = channel;
  data_channel_->RegisterObserver(this);
  RTC_LOG(LS_INFO) << "DataChannel received and observer registered"
                   << " - label: " << data_channel_->label();
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
  const Peers& peers = signaling_client_.peers();
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
  if (observer_) observer_->OnEngineEvent(BuildPeerListJson(signaling_client_.peers()));
}

void WebRTCEngine::OnPeerDisconnected(int id) {
  RTC_LOG(LS_INFO) << __FUNCTION__;
  if (id == peer_id_) {
    RTC_LOG(LS_INFO) << "Our peer disconnected";
    // Phase 2 confirmation — must be sent before cleanup.
    signaling_client_.SendHangUpConfirm();
    if (observer_) observer_->OnEngineEvent(R"({"event":"call_disconnected"})");
    if (observer_) observer_->OnEngineEvent(BuildPeerListJson(signaling_client_.peers()));

    // Sign out + reconnect: fully reset signaling state so the second
    // call starts from a clean slate (no residual call_partner / hangup
    // state on the server side).
    std::string saved_server = server_;
    int saved_port = server_port_;
    auto pc = std::move(peer_connection_);
    auto f = std::move(factory_);
    StopLocalShmRenderer();
    StopRemoteShmRenderer();
    StopRemoteAudioShmRenderer();
    data_channel_ = nullptr;
    peer_id_ = -1;
    loopback_ = false;
    signaling_client_.Close();
    signaling_thread_->PostTask([this, pc = std::move(pc), f = std::move(f)]() mutable {
      while (!pending_messages_.empty()) {
        delete pending_messages_.front();
        pending_messages_.pop_front();
      }
      if (media_)
        media_->Shutdown();
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
    if (observer_) observer_->OnEngineEvent(BuildPeerListJson(signaling_client_.peers()));
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
      signaling_client_.SignOut();
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

  if (!pending_messages_.empty() && !signaling_client_.IsSendingMessage()) {
    std::string* msg = pending_messages_.front();
    pending_messages_.pop_front();

    if (!signaling_client_.SendToPeer(peer_id_, *msg) && peer_id_ != -1) {
      RTC_LOG(LS_ERROR) << "SendToPeer failed";
      DisconnectFromServer();
    }
    delete msg;
  }

  if (!peer_connection_)
    peer_id_ = -1;

  // If a hangup was deferred because the control socket was busy, send it now
  if (pending_hangup_peer_id_ != -1 && !signaling_client_.IsSendingMessage()) {
    RTC_LOG(LS_INFO) << "Sending deferred BYE to peer " << pending_hangup_peer_id_;
    signaling_client_.SendHangUp(pending_hangup_peer_id_);
    pending_hangup_peer_id_ = -1;
  }
}

void WebRTCEngine::OnServerConnectionFailure() {
  std::string error_msg = "Failed to connect to " + server_;
  RTC_LOG(LS_ERROR) << error_msg;
  if (observer_) observer_->OnEngineEvent(R"({"event":"server_connection_failed","error":")" +
            error_msg + R"("})");
}

// ==================== DataChannelObserver ====================

void WebRTCEngine::OnStateChange() {
  if (data_channel_) {
    const char* state_str =
        DataChannelStateToString(data_channel_->state());
    RTC_LOG(LS_INFO) << "DataChannel state: " << state_str;
    if (observer_) observer_->OnEngineEvent(std::string(R"({"event":"data_channel_state","state":")") +
              state_str + R"("})");
  }
}

void WebRTCEngine::OnMessage(const webrtc::DataBuffer& buffer) {
  RTC_LOG(LS_INFO) << "DataChannel message received";
  std::string text(buffer.data.data<char>(), buffer.data.size());
  if (observer_) observer_->OnEngineEvent(R"({"event":"data_received","text":")" +
            EscapeJsonString(text) + R"("})");
}

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

  if (!media_) {
    media_ = std::make_unique<MediaManager>(env_, worker_thread_.get());
    media_->SetEventCallback([this](const std::string& json) {
      if (observer_) observer_->OnEngineEvent(json);
    });
  }

  if (!media_->GetADM()) {
    if (!media_->CreateAudioDeviceModule()) {
      RTC_LOG(LS_ERROR) << "Failed to create AudioDeviceModule";
      return false;
    }
  }

  webrtc::PeerConnectionFactoryDependencies deps;
  deps.network_thread = network_thread_.get();
  deps.worker_thread = worker_thread_.get();
  deps.signaling_thread = signaling_thread_.get();
  deps.env = env_;
  deps.adm = media_->GetADM();
  deps.audio_encoder_factory = webrtc::CreateBuiltinAudioEncoderFactory();
  deps.audio_decoder_factory = webrtc::CreateBuiltinAudioDecoderFactory();
  deps.video_encoder_factory =
      std::make_unique<webrtc::VideoEncoderFactoryTemplate<
          webrtc::LibvpxVp8EncoderTemplateAdapter,
          webrtc::LibvpxVp9EncoderTemplateAdapter,
          webrtc::OpenH264EncoderTemplateAdapter,
          webrtc::LibaomAv1EncoderTemplateAdapter>>();
  deps.video_decoder_factory =
      std::make_unique<webrtc::VideoDecoderFactoryTemplate<
          webrtc::LibvpxVp8DecoderTemplateAdapter,
          webrtc::LibvpxVp9DecoderTemplateAdapter,
          webrtc::OpenH264DecoderTemplateAdapter,
          webrtc::Dav1dDecoderTemplateAdapter>>();
  webrtc::EnableMedia(deps);
  factory_ =
      webrtc::CreateModularPeerConnectionFactory(std::move(deps));

  if (!factory_) {
    RTC_LOG(LS_ERROR) << "Failed to initialize PeerConnectionFactory";
    DeletePeerConnection();
    return false;
  }

  if (!CreatePeerConnection()) {
    RTC_LOG(LS_ERROR) << "CreatePeerConnection failed";
    DeletePeerConnection();
  }

  AddTracks();
  AddDataChannel();

  return peer_connection_ != nullptr;
}

bool WebRTCEngine::CreatePeerConnection() {
  RTC_DCHECK(factory_);
  RTC_DCHECK(!peer_connection_);

  webrtc::PeerConnectionInterface::RTCConfiguration config;
  config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;

  webrtc::PeerConnectionInterface::IceServer stun_server;
  stun_server.uri = GetSTUNServer();
  config.servers.push_back(stun_server);

  webrtc::PeerConnectionInterface::IceServer turn_server;
  turn_server.uri = GetTURNServer();
  turn_server.username = GetTurnUserName();
  turn_server.password = GetTurnPassword();
  config.servers.push_back(turn_server);

  webrtc::PeerConnectionDependencies pc_dependencies(this);
  auto error_or_peer_connection =
      factory_->CreatePeerConnectionOrError(
          config, std::move(pc_dependencies));
  if (error_or_peer_connection.ok()) {
    peer_connection_ = std::move(error_or_peer_connection.value());
    connection_active_.store(true, std::memory_order_release);
  }
  return peer_connection_ != nullptr;
}

void WebRTCEngine::DeletePeerConnection() {
  connection_active_.store(false, std::memory_order_release);
  // Clear pending messages first to prevent stale ICE candidates from being sent
  while (!pending_messages_.empty()) {
    delete pending_messages_.front();
    pending_messages_.pop_front();
  }
  StopLocalShmRenderer();
  StopRemoteShmRenderer();
  StopRemoteAudioShmRenderer();
  data_channel_ = nullptr;
  peer_connection_->Close();
  peer_connection_ = nullptr;
  factory_ = nullptr;
  peer_id_ = -1;
  loopback_ = false;
}

void WebRTCEngine::AddTracks() {
  // Resolve audio source: SHM (shared memory) or ADM (default)
  webrtc::scoped_refptr<webrtc::AudioSourceInterface> shm_audio_source;
  std::string audio_source = absl::GetFlag(FLAGS_audio_source);
  if (audio_source == "shm") {
    shm_audio_source = ShmAudioSource::Create(
        shm_audio_cap_key_path(), SHM_AUDIO_CAP_PROJ_ID);
    if (!shm_audio_source) {
      RTC_LOG(LS_ERROR)
          << "Failed to create ShmAudioSource, falling back to ADM";
    }
  }

  if (!media_->AddTracks(factory_.get(), peer_connection_.get(),
                         shm_audio_source.get()))
    return;

  // Wire local SHM renderer to the video track created by MediaManager
  auto senders = peer_connection_->GetSenders();
  for (auto& sender : senders) {
    if (sender->track() &&
        sender->track()->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
      auto* video_track =
          static_cast<webrtc::VideoTrackInterface*>(sender->track().get());
      StartLocalShmRenderer(video_track);
      break;
    }
  }
}

void WebRTCEngine::AddDataChannel() {
  if (!peer_connection_) {
    RTC_LOG(LS_WARNING) << "AddDataChannel: no peer connection";
    return;
  }
  if (data_channel_) {
    RTC_LOG(LS_WARNING) << "AddDataChannel: data channel already exists";
    return;
  }

  webrtc::DataChannelInit config;
  config.ordered = true;
  config.negotiated = true;
  config.id = 0;

  auto dc_or_error = peer_connection_->CreateDataChannelOrError("chat", &config);
  if (!dc_or_error.ok()) {
    RTC_LOG(LS_ERROR) << "Failed to create DataChannel: "
                      << dc_or_error.error().message();
    return;
  }

  data_channel_ = std::move(dc_or_error.value());
  data_channel_->RegisterObserver(this);
  RTC_LOG(LS_INFO) << "DataChannel created - label: " << data_channel_->label()
                   << " - state: " << data_channel_->state();
}

void WebRTCEngine::SendMessage(const std::string& json_object) {
  std::string* msg = new std::string(json_object);
  pending_messages_.push_back(msg);

  // If no message is currently being sent, pop and send the front now.
  // Otherwise, the pending message will be picked up by OnMessageSent when
  // the current send completes.
  if (!signaling_client_.IsSendingMessage()) {
    msg = pending_messages_.front();
    pending_messages_.pop_front();
    if (!signaling_client_.SendToPeer(peer_id_, *msg) && peer_id_ != -1) {
      RTC_LOG(LS_ERROR) << "SendToPeer failed";
      DisconnectFromServer();
    }
    delete msg;
  }
}

// ==================== SHM Renderers ====================

void WebRTCEngine::StartLocalShmRenderer(webrtc::VideoTrackInterface* track) {
  if (local_video_sink_) {
    RTC_LOG(LS_WARNING) << "Local SHM renderer already started";
    return;
  }
  local_video_sink_ = std::make_unique<ShmVideoSink>(shm_key_path() + "_local",
                                                       SHM_PROJ_ID + 1);
  track->AddOrUpdateSink(local_video_sink_.get(), webrtc::VideoSinkWants());
  RTC_LOG(LS_INFO) << "Local SHM renderer started";
}

void WebRTCEngine::StopLocalShmRenderer() {
  if (local_video_sink_) {
    // The sink is removed from the track before destruction.
    local_video_sink_.reset();
    RTC_LOG(LS_INFO) << "Local SHM renderer stopped";
  }
}

void WebRTCEngine::StartRemoteShmRenderer(webrtc::VideoTrackInterface* track) {
  if (remote_video_sink_) {
    RTC_LOG(LS_WARNING) << "Remote SHM renderer already started";
    return;
  }
  remote_video_sink_ = std::make_unique<ShmVideoSink>(shm_key_path() + "_remote",
                                                        SHM_PROJ_ID + 2);
  track->AddOrUpdateSink(remote_video_sink_.get(), webrtc::VideoSinkWants());
  RTC_LOG(LS_INFO) << "Remote SHM renderer started";
}

void WebRTCEngine::StopRemoteShmRenderer() {
  if (remote_video_sink_) {
    remote_video_sink_.reset();
    RTC_LOG(LS_INFO) << "Remote SHM renderer stopped";
  }
}

void WebRTCEngine::StartRemoteAudioShmRenderer(
    webrtc::AudioTrackInterface* track) {
  if (remote_audio_sink_) {
    RTC_LOG(LS_WARNING) << "Remote audio SHM renderer already started";
    return;
  }
  remote_audio_sink_ = std::make_unique<ShmAudioSink>(
      shm_audio_playout_key_path(), SHM_AUDIO_PLAYOUT_PROJ_ID);
  track->AddSink(remote_audio_sink_.get());
  RTC_LOG(LS_INFO) << "Remote audio SHM renderer started";
}

void WebRTCEngine::StopRemoteAudioShmRenderer() {
  if (remote_audio_sink_) {
    remote_audio_sink_.reset();
    RTC_LOG(LS_INFO) << "Remote audio SHM renderer stopped";
  }
}

#pragma GCC diagnostic pop
