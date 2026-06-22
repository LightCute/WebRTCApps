// webrtc_engine.h — platform-independent WebRTC call engine.
// Depends on injected interfaces (IMediaPipeline, IPcFactory, IIpcServer).
#ifndef APPS_PEERCONNECTION_ENGINE_WEBRTC_ENGINE_H_
#define APPS_PEERCONNECTION_ENGINE_WEBRTC_ENGINE_H_

#include <atomic>
#include <deque>
#include <memory>
#include <string>

#include "api/data_channel_interface.h"
#include "api/environment/environment.h"
#include "api/jsep.h"
#include "api/media_stream_interface.h"
#include "api/peer_connection_interface.h"
#include "api/rtc_error.h"
#include "api/scoped_refptr.h"
#include "api/task_queue/pending_task_safety_flag.h"
#include "api/video/video_frame.h"
#include "apps/peerconnection/engine/data_channel_manager.h"
#include "apps/peerconnection/engine/engine_controller.h"
#include "apps/peerconnection/engine/ipc_server_interface.h"
#include "apps/peerconnection/engine/media_pipeline_interface.h"
#include "apps/peerconnection/engine/pc_factory_interface.h"
#include "apps/peerconnection/engine/signaling_interface.h"
#include "rtc_base/thread.h"

class WebRTCEngine : public EngineController,
                     public webrtc::PeerConnectionObserver,
                     public webrtc::CreateSessionDescriptionObserver,
                     public PeerConnectionClientObserver {
 public:
  explicit WebRTCEngine(const webrtc::Environment& env);
  ~WebRTCEngine() override;

  // ---- Dependency injection (call before Init) ----
  void SetMediaPipeline(std::unique_ptr<IMediaPipeline> pipeline);
  void SetPcFactory(std::unique_ptr<IPcFactory> factory);
  void SetIpcServer(std::unique_ptr<IIpcServer> server);
  void SetSignaling(std::unique_ptr<SignalingInterface> signaling);

  // EngineController implementation (thread-safe)
  void RegisterObserver(EngineObserver* observer) override;
  void UnregisterObserver() override;
  void ConnectToServer(const std::string& server, int port) override;
  void DisconnectFromServer() override;
  void ConnectToPeer(int peer_id) override;
  void HangUp() override;
  void SendData(const std::string& text) override;
  void GetLocalSdp() override;
  bool connection_active() const override;

  // Lifecycle
  bool Init();
  void Shutdown();

  // RefCountInterface
  void AddRef() const override {}
  webrtc::RefCountReleaseStatus Release() const override {
    return webrtc::RefCountReleaseStatus::kOtherRefsRemained;
  }

  // Accessors for platform main
  webrtc::Thread* signaling_thread() { return signaling_thread_.get(); }

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

 private:
  bool InitializePeerConnection();
  void DeletePeerConnection();
  void AddTracks();
  void AddDataChannel();
  void SendMessage(const std::string& json_object);

  void ConnectToServerImpl(const std::string& server, int port);
  void DisconnectFromServerImpl();
  void ConnectToPeerImpl(int peer_id);
  void HangUpImpl();
  void SendDataImpl(const std::string& text);
  void GetLocalSdpImpl();

  // ---- Injected dependencies ----
  std::unique_ptr<IMediaPipeline> pipeline_;
  std::unique_ptr<IPcFactory> pc_factory_;
  std::unique_ptr<IIpcServer> ipc_server_;
  std::unique_ptr<SignalingInterface> signaling_;

  EngineObserver* observer_ = nullptr;
  std::atomic<bool> connection_active_{false};
  const webrtc::Environment env_;
  webrtc::ScopedTaskSafety safety_;

  std::unique_ptr<webrtc::Thread> network_thread_;
  std::unique_ptr<webrtc::Thread> worker_thread_;
  std::unique_ptr<webrtc::Thread> signaling_thread_;

  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection_;
  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory_;
  std::unique_ptr<DataChannelManager> dc_manager_;

  int peer_id_ = -1;
  int pending_hangup_peer_id_ = -1;
  bool loopback_ = false;
  bool collecting_sdp_ = false;
  std::string server_;
  int server_port_ = 8888;
  std::deque<std::string*> pending_messages_;
};

#endif  // APPS_PEERCONNECTION_ENGINE_WEBRTC_ENGINE_H_
