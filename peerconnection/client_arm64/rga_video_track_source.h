#ifndef APPS_PEERCONNECTION_CLIENT_RGA_VIDEO_TRACK_SOURCE_H_
#define APPS_PEERCONNECTION_CLIENT_RGA_VIDEO_TRACK_SOURCE_H_

#include <memory>
#include <mutex>
#include <vector>

#include "api/scoped_refptr.h"
#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"
#include "api/video/video_source_interface.h"
#include "api/video_codecs/video_encoder.h"
#include "modules/video_capture/raw_video_sink_interface.h"
#include "modules/video_capture/video_capture.h"
#include "modules/video_capture/video_capture_defines.h"
#include "pc/video_track_source.h"
#include "rtc_base/thread.h"

class DmaBufPool;
struct ShmCtrlBlock;

namespace webrtc {
class Environment;
class TaskQueueFactory;
}

// Replaces CapturerTrackSource with hardware-accelerated capture:
// VCM V4L2 -> RawVideoSinkInterface -> RGA YUYV->NV12 (DMA) ->
// NV12 VideoFrame -> MPP H264 encoder.
// Also outputs local preview via RGA NV12->I420 -> DmaBufPool -> Unix socket.
class RgaVideoTrackSource : public webrtc::VideoTrackSource,
                            public webrtc::RawVideoSinkInterface {
 public:
  static webrtc::scoped_refptr<RgaVideoTrackSource> Create(
      webrtc::TaskQueueFactory& task_queue_factory,
      int device_idx = -1);

  ~RgaVideoTrackSource() override;

  webrtc::VideoSourceInterface<webrtc::VideoFrame>* source() override {
    return this;
  }

  void AddOrUpdateSink(webrtc::VideoSinkInterface<webrtc::VideoFrame>* sink,
                       const webrtc::VideoSinkWants& wants) override;
  void RemoveSink(webrtc::VideoSinkInterface<webrtc::VideoFrame>* sink) override;

  int32_t OnRawFrame(uint8_t* videoFrame, size_t videoFrameLength,
                     const webrtc::VideoCaptureCapability& frameInfo,
                     webrtc::VideoRotation rotation,
                     int64_t captureTime) override;

  void SetLocalPreview(DmaBufPool* pool, ShmCtrlBlock* ctrl);
  void SetCapturePool(DmaBufPool* pool, ShmCtrlBlock* ctrl);

 protected:
  explicit RgaVideoTrackSource();

 private:
  bool Init(webrtc::TaskQueueFactory& task_queue_factory, int device_idx);
  bool LoadRga();

  void* rga_lib_ = nullptr;
  int (*rga_blit_)(void*, void*, void*) = nullptr;

  webrtc::scoped_refptr<webrtc::VideoCaptureModule> vcm_;
  int width_ = 640, height_ = 480;

  std::vector<uint8_t> nv12_buf_;

  webrtc::VideoSinkInterface<webrtc::VideoFrame>* sink_ = nullptr;
  webrtc::VideoSinkWants wants_;

  DmaBufPool* preview_pool_ = nullptr;
  ShmCtrlBlock* preview_ctrl_ = nullptr;

  DmaBufPool* capture_pool_ = nullptr;
  ShmCtrlBlock* capture_ctrl_ = nullptr;

  std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> device_info_;
};

// Extract DMA-BUF fd from an NV12 VideoFrame backed by Nv12DmaBufBuffer.
// Returns -1 if the frame is a regular NV12Buffer (CPU path) or no fd.
int GetNv12DmaBufFd(const webrtc::VideoFrame& frame);

#endif
