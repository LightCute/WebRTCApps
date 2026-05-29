#ifndef APPS_PEERCONNECTION_CLIENT_MEDIA_PIPELINE_H_
#define APPS_PEERCONNECTION_CLIENT_MEDIA_PIPELINE_H_

#include <functional>
#include <memory>
#include <string>

#include "api/audio/audio_device.h"
#include "api/environment/environment.h"
#include "api/media_stream_interface.h"
#include "api/peer_connection_interface.h"
#include "api/scoped_refptr.h"

#include "apps/peerconnection/client_arm64/shm_video_renderer.h"
#include "apps/peerconnection/client_arm64/video_frame_shm_ctrl.h"
#include "apps/peerconnection/client_arm64/dma_buf_pool.h"
#include "apps/peerconnection/client_arm64/dma_buf_server.h"
#include "apps/peerconnection/client_arm64/rga_decoded_sink.h"
#include "apps/peerconnection/client_arm64/rga_video_track_source.h"
#include "apps/peerconnection/client_arm64/rk_mpp_codec.h"
#include "rtc_base/thread.h"

// Step 1: ADM. Step 2: renderers. Later: sources, device state.
class MediaPipeline {
 public:
  MediaPipeline(const webrtc::Environment& env,
                webrtc::Thread* worker_thread);
  ~MediaPipeline();

  void SetDecoder(webrtc::MppH264Decoder* decoder) { decoder_ = decoder; }

  // ---- ADM ----
  bool CreateAudioDeviceModule();
  webrtc::AudioDeviceModule* adm() const { return adm_.get(); }
  void FixupAudioDeviceSelection();

  // ---- Sources ----
  webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> CreateVideoSource();
  webrtc::AudioSourceInterface* CreateAudioSource(
      webrtc::PeerConnectionFactoryInterface* factory);

  // ---- Events ----
  using EventCallback = std::function<void(const std::string& json)>;
  void SetEventCallback(EventCallback cb) { event_cb_ = std::move(cb); }

  // ---- Device management ----
  void SetVideoDevice(int device_idx);
  void SetAudioMuted(bool muted);
  void SetVideoPaused(bool paused, webrtc::PeerConnectionInterface* pc);
  void QueryDevices();

  void set_video_device_idx(int idx) { video_device_idx_ = idx; }
  void set_audio_input_device_idx(int idx) { audio_input_device_idx_ = idx; }

  // ---- Renderers ----
  void StartLocalRenderer(webrtc::VideoTrackInterface* track);
  void StopLocalRenderer();
  void StartRemoteRenderer(webrtc::VideoTrackInterface* track);
  void StopRemoteRenderer();
  // ---- Lifecycle ----
  void Shutdown();

 private:
  const webrtc::Environment& env_;
  webrtc::Thread* const worker_thread_;
  webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm_;
  webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> video_source_;
  webrtc::scoped_refptr<webrtc::AudioSourceInterface> audio_source_;

  std::unique_ptr<ShmVideoRenderer> local_renderer_;
  std::unique_ptr<ShmVideoRenderer> remote_renderer_;

  // DMA-BUF hardware rendering (RK3588)
  std::unique_ptr<DmaBufPool> local_dma_pool_;
  std::unique_ptr<DmaBufPool> remote_dma_pool_;
  std::unique_ptr<DmaBufServer> local_dma_server_;
  std::unique_ptr<DmaBufServer> remote_dma_server_;
  std::unique_ptr<RgaDecodedSink> remote_rga_sink_;
  ShmMultiCtrlBlock* local_ctrl_ = nullptr;
  ShmMultiCtrlBlock* remote_ctrl_ = nullptr;
  bool use_rga_source_ = false;

  // Capture pool (NV12 DMA-BUF for zero-copy encode)
  std::unique_ptr<DmaBufPool> capture_pool_;
  ShmCtrlBlock* capture_ctrl_ = nullptr;
  webrtc::MppH264Decoder* decoder_ = nullptr;

  EventCallback event_cb_;
  int video_device_idx_ = -1;
  int audio_input_device_idx_ = -1;
};

#endif  // APPS_PEERCONNECTION_CLIENT_MEDIA_PIPELINE_H_
