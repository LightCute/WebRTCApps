// webrtc_engine.h
#ifndef APPS_PEERCONNECTION_CLIENT_WEBRTC_ENGINE_H_
#define APPS_PEERCONNECTION_CLIENT_WEBRTC_ENGINE_H_

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "api/audio/audio_device.h"
#include "api/data_channel_interface.h"
#include "api/environment/environment.h"
#include "api/jsep.h"
#include "api/media_stream_interface.h"
#include "api/peer_connection_interface.h"
#include "api/rtc_error.h"
#include "api/scoped_refptr.h"
#include "api/task_queue/pending_task_safety_flag.h"
#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"
#include "apps/peerconnection/client/engine_controller.h"
#include "apps/peerconnection/client/shm_audio_capturer.h"
#include "apps/peerconnection/client/shm_audio_renderer.h"
#include "apps/peerconnection/client/shm_video_renderer.h"
#include "apps/peerconnection/client/signaling_interface.h"
#include "rtc_base/thread.h"

class WebRTCEngine : public EngineController,
                     public webrtc::PeerConnectionObserver,
                     public webrtc::CreateSessionDescriptionObserver,
                     public PeerConnectionClientObserver,
                     public webrtc::DataChannelObserver {
 public:
  WebRTCEngine(const webrtc::Environment& env);
  ~WebRTCEngine() override;

  // EngineController implementation (thread-safe, callable from any thread)
  void RegisterObserver(EngineObserver* observer) override;
  void UnregisterObserver() override;
  void ConnectToServer(const std::string& server, int port) override;
  void DisconnectFromServer() override;
  void ConnectToPeer(int peer_id) override;
  void HangUp() override;
  void SetAudioMuted(bool muted) override;
  void SetVideoPaused(bool paused) override;
  void SendData(const std::string& text) override;
  void QueryDevices() override;
  void SetVideoDevice(int device_idx) override;
  void SetAudioInputDevice(int device_idx) override;
  bool connection_active() const override;

  // Lifecycle (called by main.cc, not part of EngineController)
  bool Init();
  void Shutdown();

  // RefCountInterface (required by CreateSessionDescriptionObserver)
  void AddRef() const override {}
  webrtc::RefCountReleaseStatus Release() const override {
    return webrtc::RefCountReleaseStatus::kOtherRefsRemained;
  }

 protected:
  // PeerConnectionObserver
  void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override {}
  void OnAddTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
                  const std::vector<webrtc::scoped_refptr<webrtc::MediaStreamInterface>>& streams) override;
  void OnRemoveTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) override;
  void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) override;
  void OnRenegotiationNeeded() override {}
  void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState) override;
  void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState) override {}
  void OnIceCandidate(const webrtc::IceCandidate* candidate) override;
  void OnIceCandidateRemoved(const webrtc::IceCandidate* candidate) override {}
  void OnIceConnectionReceivingChange(bool) override {}

  // CreateSessionDescriptionObserver
  void OnSuccess(webrtc::SessionDescriptionInterface* desc) override;
  void OnFailure(webrtc::RTCError error) override;

  // PeerConnectionClientObserver
  void OnSignedIn() override;
  void OnDisconnected() override;
  void OnPeerConnected(int id, const std::string& name) override;
  void OnPeerDisconnected(int id) override;
  void OnPeerBusy(int peer_id) override;
  void OnMessageFromPeer(int peer_id, const std::string& message) override;
  void OnMessageSent(int err) override;
  void OnServerConnectionFailure() override;

  // DataChannelObserver
  void OnStateChange() override;
  void OnMessage(const webrtc::DataBuffer& buffer) override;
  void OnBufferedAmountChange(uint64_t) override {}
  bool IsOkToCallOnTheNetworkThread() override { return false; }

 private:
  // Internal helpers (ported from Conductor)
  bool InitializePeerConnection();
  bool CreatePeerConnection();
  void DeletePeerConnection();
  void AddTracks();
  void AddDataChannel();
  void SendMessage(const std::string& json_object);

  // EngineController Impl helpers — must be called on signaling thread
  void ConnectToServerImpl(const std::string& server, int port);
  void DisconnectFromServerImpl();
  void ConnectToPeerImpl(int peer_id);
  void HangUpImpl();
  void SetAudioMutedImpl(bool muted);
  void SetVideoPausedImpl(bool paused);
  void SendDataImpl(const std::string& text);
  void QueryDevicesImpl();
  void SetVideoDeviceImpl(int device_idx);
  void SetAudioInputDeviceImpl(int device_idx);

  // SHM renderers
  void StartLocalShmRenderer(webrtc::VideoTrackInterface* track);
  void StopLocalShmRenderer();
  void StartRemoteShmRenderer(webrtc::VideoTrackInterface* track);
  void StopRemoteShmRenderer();

  // Audio SHM renderer
  void StartRemoteAudioShmRenderer(webrtc::AudioTrackInterface* track);
  void StopRemoteAudioShmRenderer();

  EngineObserver* observer_ = nullptr;
  std::atomic<bool> connection_active_{false};
  const webrtc::Environment env_;
  webrtc::ScopedTaskSafety safety_;

  // WebRTC threads
  std::unique_ptr<webrtc::Thread> network_thread_;
  std::unique_ptr<webrtc::Thread> worker_thread_;
  std::unique_ptr<webrtc::Thread> signaling_thread_;

  // WebRTC objects
  webrtc::scoped_refptr<webrtc::AudioDeviceModule> audio_device_module_;
  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection_;
  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory_;
  webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> local_video_source_;
  webrtc::scoped_refptr<webrtc::DataChannelInterface> data_channel_;

  // Signaling client (abstract interface, concrete impl = PeerConnectionClient)
  std::unique_ptr<SignalingInterface> signaling_;

  // SHM renderers
  std::unique_ptr<ShmVideoRenderer> local_video_renderer_;
  std::unique_ptr<ShmVideoRenderer> remote_video_renderer_;

  // SHM audio
  std::unique_ptr<ShmAudioRenderer> remote_audio_renderer_;
  webrtc::scoped_refptr<webrtc::AudioSourceInterface> local_audio_source_;

  // State
  int peer_id_ = -1;
  int pending_hangup_peer_id_ = -1;
  bool loopback_ = false;
  std::string server_;
  int server_port_ = 8888;
  std::deque<std::string*> pending_messages_;

  // Device state
  int current_video_device_idx_ = -1;
  int current_audio_input_device_idx_ = -1;
};

#endif  // APPS_PEERCONNECTION_CLIENT_WEBRTC_ENGINE_H_
