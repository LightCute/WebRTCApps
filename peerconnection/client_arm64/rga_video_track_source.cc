#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunsafe-buffer-usage"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"

#include "apps/peerconnection/client_arm64/rga_video_track_source.h"

#include <dlfcn.h>
#include <pthread.h>
#include <cstdio>
#include <cstring>

#include "api/make_ref_counted.h"
#include "api/video/nv12_buffer.h"
#include "apps/peerconnection/client_arm64/dma_buf_pool.h"
#include "apps/peerconnection/client_arm64/shm_common.h"
#include "modules/video_capture/video_capture_factory.h"
#include "rtc_base/logging.h"

extern "C" {
#include "rga.h"
#include "drmrga.h"
}

// NV12 buffer backed by a DMA-BUF fd. Carries the fd through the WebRTC
// frame pipeline so MPP encoder can import it for zero-copy encoding.
class Nv12DmaBufBuffer : public webrtc::NV12Buffer {
 public:
  Nv12DmaBufBuffer(int width, int height, int fd)
      : NV12Buffer(width, height, width, width), fd_(fd) {}

  int fd() const { return fd_; }

  static webrtc::scoped_refptr<Nv12DmaBufBuffer> Create(
      int width, int height, int fd) {
    return webrtc::make_ref_counted<Nv12DmaBufBuffer>(width, height, fd);
  }

 private:
  int fd_;
};

// Extract NV12 dma-buf fd from a VideoFrame if backed by Nv12DmaBufBuffer.
// Returns -1 for regular NV12Buffer (CPU path).
int GetNv12DmaBufFd(const webrtc::VideoFrame& frame) {
  // dynamic_cast unavailable with -fno-rtti. For the DMA-BUF capture→encode
  // path, the MPP encoder import must be wired on the RK3588 board using a
  // RTTI-free dispatch — either a side-channel fd map or a type-id in NV12Buffer.
  (void)frame;
  return -1;
}

RgaVideoTrackSource::RgaVideoTrackSource()
    : VideoTrackSource(/*remote=*/false) {}

RgaVideoTrackSource::~RgaVideoTrackSource() {
  if (vcm_) { vcm_->StopCapture(); vcm_->DeRegisterCaptureDataCallback(); }
  if (rga_lib_) dlclose(rga_lib_);
}

bool RgaVideoTrackSource::LoadRga() {
  rga_lib_ = dlopen("librga.so", RTLD_NOW | RTLD_GLOBAL);
  if (!rga_lib_) return false;
  rga_blit_ = reinterpret_cast<int(*)(void*,void*,void*)>(
      dlsym(rga_lib_, "c_RkRgaBlit"));
  return rga_blit_ != nullptr;
}

bool RgaVideoTrackSource::Init(webrtc::TaskQueueFactory& task_queue_factory,
                                int device_idx) {
  if (!LoadRga()) {
    RTC_LOG(LS_WARNING) << "RgaVideoTrackSource: librga not available";
    return false;
  }

  device_info_.reset(webrtc::VideoCaptureFactory::CreateDeviceInfo());
  if (!device_info_) return false;

  int ndev = device_info_->NumberOfDevices();
  if (ndev <= 0) return false;

  int target = (device_idx >= 0 && device_idx < ndev) ? device_idx : 0;
  char name[256], uid[256];
  if (device_info_->GetDeviceName(target, name, sizeof(name), uid, sizeof(uid)) != 0)
    return false;

  vcm_ = webrtc::VideoCaptureFactory::Create(uid);
  if (!vcm_) return false;

  vcm_->RegisterCaptureDataCallback(
      static_cast<webrtc::RawVideoSinkInterface*>(this));

  webrtc::VideoCaptureCapability cap;
  cap.width = width_; cap.height = height_;
  cap.maxFPS = 30;
  cap.videoType = webrtc::VideoType::kYUY2;
  cap.interlaced = false;

  if (vcm_->StartCapture(cap) != 0) return false;
  return vcm_->CaptureStarted();
}

webrtc::scoped_refptr<RgaVideoTrackSource> RgaVideoTrackSource::Create(
    webrtc::TaskQueueFactory& task_queue_factory, int device_idx) {
  auto src = webrtc::make_ref_counted<RgaVideoTrackSource>();
  if (!src->Init(task_queue_factory, device_idx)) return nullptr;
  return src;
}

void RgaVideoTrackSource::AddOrUpdateSink(
    webrtc::VideoSinkInterface<webrtc::VideoFrame>* sink,
    const webrtc::VideoSinkWants& wants) {
  sink_ = sink;
  wants_ = wants;
}

void RgaVideoTrackSource::RemoveSink(
    webrtc::VideoSinkInterface<webrtc::VideoFrame>* sink) {
  if (sink_ == sink) sink_ = nullptr;
}

void RgaVideoTrackSource::SetLocalPreview(DmaBufPool* pool, ShmCtrlBlock* ctrl) {
  preview_pool_ = pool;
  preview_ctrl_ = ctrl;
}

void RgaVideoTrackSource::SetCapturePool(DmaBufPool* pool, ShmCtrlBlock* ctrl) {
  capture_pool_ = pool;
  capture_ctrl_ = ctrl;
}

int32_t RgaVideoTrackSource::OnRawFrame(uint8_t* videoFrame,
                                         size_t videoFrameLength,
                                         const webrtc::VideoCaptureCapability& frameInfo,
                                         webrtc::VideoRotation rotation,
                                         int64_t captureTime) {
  int w = frameInfo.width, h = frameInfo.height;
  if (w <= 0 || h <= 0) return 0;

  int ys = w * h, uvs = ys / 2;
  webrtc::scoped_refptr<webrtc::NV12Buffer> nv12;

  // ── Zero-copy DMA-BUF capture path ──
  if (capture_pool_ && capture_ctrl_) {
    pthread_mutex_lock(&capture_ctrl_->mtx);
    while (capture_ctrl_->frame_count >= RING_BUFFER_CNT) {
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_nsec += 100 * 1000000;
      if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
      pthread_cond_timedwait(&capture_ctrl_->cv_can_write,
                             &capture_ctrl_->mtx, &ts);
    }
    uint32_t cap_idx = capture_ctrl_->w_idx;
    pthread_mutex_unlock(&capture_ctrl_->mtx);

    int cap_fd = capture_pool_->GetFd(cap_idx);

    rga_info_t src{};
    memset(&src, 0, sizeof(src));
    src.virAddr = videoFrame;
    src.format = RK_FORMAT_YUYV_422;
    src.rect.width = w; src.rect.height = h;
    src.rect.wstride = w; src.rect.hstride = h;
    src.rect.format = RK_FORMAT_YUYV_422;
    src.mmuFlag = 1; src.sync_mode = 0;

    rga_info_t dst{};
    memset(&dst, 0, sizeof(dst));
    dst.fd = cap_fd;
    dst.format = RK_FORMAT_YCbCr_420_SP;
    dst.rect.width = w; dst.rect.height = h;
    dst.rect.wstride = w; dst.rect.hstride = h;
    dst.rect.format = RK_FORMAT_YCbCr_420_SP;
    dst.mmuFlag = 1; dst.sync_mode = 0;

    if (rga_blit_(&src, &dst, nullptr) == 0) {
      nv12 = Nv12DmaBufBuffer::Create(w, h, cap_fd);
    }
    // On RGA failure, nv12 stays null → fall through to CPU path below
  }

  // ── CPU fallback path ──
  if (!nv12) {
    size_t nv12_size = ys + uvs;
    if (nv12_buf_.size() < nv12_size) nv12_buf_.resize(nv12_size);

    // RGA: YUYV -> NV12 (DMA, zero CPU)
    {
      rga_info_t src;
      memset(&src, 0, sizeof(src));
      src.virAddr = videoFrame;
      src.format = RK_FORMAT_YUYV_422;
      src.rect.width = w; src.rect.height = h;
      src.rect.wstride = w; src.rect.hstride = h;
      src.rect.format = RK_FORMAT_YUYV_422;
      src.mmuFlag = 1; src.sync_mode = 0;

      rga_info_t dst;
      memset(&dst, 0, sizeof(dst));
      dst.virAddr = nv12_buf_.data();
      dst.format = RK_FORMAT_YCbCr_420_SP;
      dst.rect.width = w; dst.rect.height = h;
      dst.rect.wstride = w; dst.rect.hstride = h;
      dst.rect.format = RK_FORMAT_YCbCr_420_SP;
      dst.mmuFlag = 1; dst.sync_mode = 0;

      if (rga_blit_(&src, &dst, nullptr) != 0) return 0;
    }

    // Build NV12 VideoFrame for encoder
    nv12 = webrtc::NV12Buffer::Create(w, h);
    memcpy(nv12->MutableDataY(), nv12_buf_.data(), ys);
    memcpy(nv12->MutableDataUV(), nv12_buf_.data() + ys, uvs);
  }

  webrtc::VideoFrame frame = webrtc::VideoFrame::Builder()
      .set_video_frame_buffer(nv12)
      .set_rotation(rotation)
      .set_timestamp_us(captureTime * 1000)
      .build();

  // Deliver to encoder sink
  if (sink_) sink_->OnFrame(frame);

  // Local preview: RGA NV12->I420 to DMA-BUF
  if (preview_pool_ && preview_ctrl_) {
    pthread_mutex_lock(&preview_ctrl_->mtx);
    while (preview_ctrl_->frame_count >= RING_BUFFER_CNT) {
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_nsec += 100 * 1000000;
      if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
      pthread_cond_timedwait(&preview_ctrl_->cv_can_write,
                             &preview_ctrl_->mtx, &ts);
    }
    uint32_t pw_idx = preview_ctrl_->w_idx;
    pthread_mutex_unlock(&preview_ctrl_->mtx);

    int dst_fd = preview_pool_->GetFd(pw_idx);

    rga_info_t src2;
    memset(&src2, 0, sizeof(src2));
    src2.virAddr = nv12_buf_.data();
    src2.format = RK_FORMAT_YCbCr_420_SP;
    src2.rect.width = w; src2.rect.height = h;
    src2.rect.wstride = w; src2.rect.hstride = h;
    src2.rect.format = RK_FORMAT_YCbCr_420_SP;
    src2.mmuFlag = 1; src2.sync_mode = 0;

    rga_info_t dst2;
    memset(&dst2, 0, sizeof(dst2));
    dst2.fd = dst_fd;
    dst2.format = RK_FORMAT_YCbCr_420_P;
    dst2.rect.width = w; dst2.rect.height = h;
    dst2.rect.wstride = w; dst2.rect.hstride = h;
    dst2.rect.format = RK_FORMAT_YCbCr_420_P;
    dst2.mmuFlag = 1; dst2.sync_mode = 0;

    if (rga_blit_(&src2, &dst2, nullptr) == 0) {
      size_t total = ys + 2 * (ys / 4);
      pthread_mutex_lock(&preview_ctrl_->mtx);
      RingVideoFrameItem& item = preview_ctrl_->ring[pw_idx];
      memset(&item.head, 0, sizeof(item.head));
      item.head.frame_len = static_cast<uint32_t>(total);
      item.head.width = static_cast<uint16_t>(w);
      item.head.height = static_cast<uint16_t>(h);
      preview_ctrl_->w_idx = (pw_idx + 1) % RING_BUFFER_CNT;
      preview_ctrl_->frame_count++;
      pthread_cond_signal(&preview_ctrl_->cv_can_read);
      pthread_mutex_unlock(&preview_ctrl_->mtx);
    } else {
      pthread_mutex_unlock(&preview_ctrl_->mtx);
    }
  }

  return 0;
}

#pragma clang diagnostic pop
#pragma GCC diagnostic pop
