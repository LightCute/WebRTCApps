// webrtc_engine.h
#ifndef APPS_PEERCONNECTION_CLIENT_WEBRTC_ENGINE_H_
#define APPS_PEERCONNECTION_CLIENT_WEBRTC_ENGINE_H_

#include <atomic>
#include <deque>
#include <functional>
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
#include "apps/peerconnection/client/peer_connection_client.h"
#include "apps/peerconnection/client/shm_video_writer.h"
#include "apps/peerconnection/client/shm_audio_writer.h"
#include "apps/peerconnection/client/shm_audio_reader.h"
#include "rtc_base/thread.h"

class WebRTCEngine : public webrtc::PeerConnectionObserver,
                     public webrtc::CreateSessionDescriptionObserver,
                     public PeerConnectionClientObserver,
                     public webrtc::DataChannelObserver {
 public:
  using EventCallback = std::function<void(const std::string& json)>;

  WebRTCEngine(const webrtc::Environment& env, EventCallback on_event);
  ~WebRTCEngine() override;

  // Lifecycle
  bool Init();
  void Shutdown();

  // Signaling
  void ConnectToServer(const std::string& server, int port);
  void DisconnectFromServer();
  void ConnectToPeer(int peer_id);
  void HangUp();

  // Media control
  void SetAudioMuted(bool muted);
  void SetVideoPaused(bool paused);

  // DataChannel
  void SendData(const std::string& text);

  // Device management
  void QueryDevices();
  void SetVideoDevice(int device_idx);
  void SetAudioInputDevice(int device_idx);

  // RefCountInterface (required by CreateSessionDescriptionObserver)
  void AddRef() const override {}
  webrtc::RefCountReleaseStatus Release() const override {
    return webrtc::RefCountReleaseStatus::kOtherRefsRemained;
  }

  // Callback can be set after construction (for daemon mode wiring)
  void SetEventCallback(EventCallback cb) { on_event_ = std::move(cb); }

  // Thread access for UnixSocketServer command dispatch
  webrtc::Thread* signaling_thread() const { return signaling_thread_.get(); }
  bool connection_active() const { return peer_connection_ != nullptr; }

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

  // SHM renderers
  void StartLocalShmRenderer(webrtc::VideoTrackInterface* track);
  void StopLocalShmRenderer();
  void StartRemoteShmRenderer(webrtc::VideoTrackInterface* track);
  void StopRemoteShmRenderer();

  // Audio SHM renderer
  void StartRemoteAudioShmRenderer(webrtc::AudioTrackInterface* track);
  void StopRemoteAudioShmRenderer();

  // Inner class: VideoSink that writes frames to SHM via dedicated IO thread
  class ShmVideoSink : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
   public:
    ShmVideoSink(const std::string& key_path, int proj_id);
    ~ShmVideoSink() override;
    void OnFrame(const webrtc::VideoFrame& frame) override;

   private:
    struct FrameData {
      std::vector<uint8_t> i420_data;
      VideoFrameHead head;
    };

    void IoLoop();

    std::unique_ptr<ShmVideoWriter> writer_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<FrameData> pending_;
    bool stopped_ = false;
    std::thread io_thread_;
  };

  // Inner class: AudioSink that writes remote audio to SHM for Qt playback
  class ShmAudioSink : public webrtc::AudioTrackSinkInterface {
   public:
    ShmAudioSink(const std::string& key_path, int proj_id);
    ~ShmAudioSink() override;
    void OnData(const void* audio_data, int bits_per_sample,
                int sample_rate, size_t number_of_channels,
                size_t number_of_frames,
                std::optional<int64_t> absolute_capture_timestamp_ms) override;

   private:
    struct PendingAudio {
      AudioFrameHead head;
      std::vector<uint8_t> data;
    };

    void IoLoop();

    std::unique_ptr<ShmAudioWriter> writer_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<PendingAudio> pending_;
    bool stopped_ = false;
    std::thread io_thread_;
  };

  // Inner class: AudioSource that reads capture audio from SHM (Qt mic input)
  class ShmAudioSource : public webrtc::AudioSourceInterface {
   public:
    static webrtc::scoped_refptr<ShmAudioSource> Create(
        const std::string& key_path, int proj_id);
    ~ShmAudioSource() override;

    void AddSink(webrtc::AudioTrackSinkInterface* sink) override;
    void RemoveSink(webrtc::AudioTrackSinkInterface* sink) override;
    void RegisterObserver(webrtc::ObserverInterface*) override {}
    void UnregisterObserver(webrtc::ObserverInterface*) override {}
    webrtc::MediaSourceInterface::SourceState state() const override {
      return webrtc::MediaSourceInterface::kLive;
    }
    bool remote() const override { return false; }

   protected:
    ShmAudioSource() = default;
    void CaptureLoop();

    ShmAudioReader reader_;
    std::mutex sinks_mutex_;
    std::vector<webrtc::AudioTrackSinkInterface*> sinks_;
    std::thread capture_thread_;
    std::atomic<bool> running_{false};
  };

  EventCallback on_event_;
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

  // Signaling client
  PeerConnectionClient signaling_client_;

  // SHM renderers
  std::unique_ptr<ShmVideoSink> local_video_sink_;
  std::unique_ptr<ShmVideoSink> remote_video_sink_;

  // SHM audio
  std::unique_ptr<ShmAudioSink> remote_audio_sink_;
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
