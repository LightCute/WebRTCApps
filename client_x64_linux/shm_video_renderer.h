#ifndef APPS_PEERCONNECTION_CLIENT_SHM_VIDEO_RENDERER_H_
#define APPS_PEERCONNECTION_CLIENT_SHM_VIDEO_RENDERER_H_

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"
#include "apps/client_x64_linux/shm_common.h"
#include "apps/client_x64_linux/shm_video_writer.h"

// Renders video frames to shared memory for an external Qt process.
// Runs a dedicated IO thread to avoid blocking the WebRTC signaling thread.
class ShmVideoRenderer : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
 public:
  ShmVideoRenderer(const std::string& key_path, int proj_id);
  ~ShmVideoRenderer() override;
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

#endif  // APPS_PEERCONNECTION_CLIENT_SHM_VIDEO_RENDERER_H_
