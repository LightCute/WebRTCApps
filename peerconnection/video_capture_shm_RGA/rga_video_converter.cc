// rga_video_converter.cc — RGA hardware YUYV→I420 via dma-buf
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunsafe-buffer-usage"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"

#include "apps/peerconnection/video_capture_shm_RGA/rga_video_converter.h"

#include <dlfcn.h>
#include <pthread.h>
#include <cstdio>
#include <cstring>

#include "apps/peerconnection/video_capture_shm_RGA/video_frame_shm_ctrl.h"
#include "apps/peerconnection/video_capture_shm_RGA/dma_buf_pool.h"
#include "rtc_base/logging.h"

// RGA headers for type definitions only.
// Actual function symbols resolved at runtime via dlopen.
extern "C" {
#include "rga.h"
#include "drmrga.h"
}

// ── RgaVideoConverter ──────────────────────────────────────────

RgaVideoConverter::RgaVideoConverter() = default;

RgaVideoConverter::~RgaVideoConverter() {
  if (fp_rga_deinit_) fp_rga_deinit_();
  if (lib_handle_) dlclose(lib_handle_);
}

bool RgaVideoConverter::Init() {
  lib_handle_ = dlopen("librga.so", RTLD_NOW | RTLD_GLOBAL);
  if (!lib_handle_) {
    RTC_LOG(LS_WARNING) << "RgaVideoConverter: dlopen librga.so failed: "
                        << dlerror();
    return false;
  }

  fp_rga_init_   = reinterpret_cast<int(*)()>(dlsym(lib_handle_, "c_RkRgaInit"));
  fp_rga_deinit_ = reinterpret_cast<void(*)()>(dlsym(lib_handle_, "c_RkRgaDeInit"));
  fp_rga_blit_   = reinterpret_cast<int(*)(void*,void*,void*)>(
                       dlsym(lib_handle_, "c_RkRgaBlit"));

  if (!fp_rga_init_ || !fp_rga_deinit_ || !fp_rga_blit_) {
    RTC_LOG(LS_WARNING) << "RgaVideoConverter: dlsym failed";
    dlclose(lib_handle_);
    lib_handle_ = nullptr;
    return false;
  }

  if (fp_rga_init_() != 0) {
    RTC_LOG(LS_WARNING) << "RgaVideoConverter: c_RkRgaInit failed";
    dlclose(lib_handle_);
    lib_handle_ = nullptr;
    return false;
  }

  loaded_ = true;
  RTC_LOG(LS_INFO) << "RgaVideoConverter: RGA hardware ready";
  return true;
}

void RgaVideoConverter::SetOutput(DmaBufPool* pool, ShmMultiCtrlBlock* ctrl) {
  pool_ = pool;
  ctrl_ = ctrl;
}

int32_t RgaVideoConverter::OnRawFrame(uint8_t* videoFrame,
                                      size_t videoFrameLength,
                                      const webrtc::VideoCaptureCapability& frameInfo,
                                      webrtc::VideoRotation rotation,
                                      int64_t captureTime) {
  if (!loaded_ || !pool_ || !ctrl_) return 0;

  int w = frameInfo.width;
  int h = frameInfo.height;
  if (w <= 0 || h <= 0) return 0;

  size_t y_size = w * h;
  size_t uv_size = ((w + 1) / 2) * ((h + 1) / 2);
  size_t total = y_size + 2 * uv_size;

  // No back-pressure: always write, overwrite oldest slot
  pthread_mutex_lock(&ctrl_->mtx);
  uint32_t w_idx = ctrl_->w_idx;

  // Seqlock: mark slot as "writing" (odd seq)
  ctrl_->seq[w_idx]++;
  __sync_synchronize();  // write barrier: seq visible before RGA blit starts
  pthread_mutex_unlock(&ctrl_->mtx);

  // Get dma-buf fd for this slot — RGA writes directly to CMA memory
  int dst_fd = pool_->GetFd(w_idx);

  // Configure source (YUYV from V4L2 MMAP)
  rga_info_t src;
  memset(&src, 0, sizeof(src));
  src.virAddr = videoFrame;
  src.format = RK_FORMAT_YUYV_422;
  src.rect.xoffset = 0;
  src.rect.yoffset = 0;
  src.rect.width = w;
  src.rect.height = h;
  src.rect.wstride = w;
  src.rect.hstride = h;
  src.rect.format = RK_FORMAT_YUYV_422;
  src.mmuFlag = 1;
  src.sync_mode = 0;

  rga_info_t dst;
  memset(&dst, 0, sizeof(dst));
  dst.fd = dst_fd;
  dst.format = RK_FORMAT_YCbCr_420_P;
  dst.rect.xoffset = 0;
  dst.rect.yoffset = 0;
  dst.rect.width = w;
  dst.rect.height = h;
  dst.rect.wstride = w;
  dst.rect.hstride = h;
  dst.rect.format = RK_FORMAT_YCbCr_420_P;
  dst.mmuFlag = 1;
  dst.sync_mode = 0;

  if (fp_rga_blit_(&src, &dst, nullptr) != 0) {
    static int err_count = 0;
    if (err_count++ < 3) {
      RTC_LOG(LS_WARNING) << "RgaVideoConverter: c_RkRgaBlit failed";
    }
    // Revert seqlock on failure
    pthread_mutex_lock(&ctrl_->mtx);
    ctrl_->seq[w_idx]--;  // restore even
    pthread_mutex_unlock(&ctrl_->mtx);
    return 0;
  }

  // RGA blit complete — write metadata and advance
  pthread_mutex_lock(&ctrl_->mtx);

  RingVideoFrameItem& item = ctrl_->ring[w_idx];
  item.head.ntp_time_ms = captureTime;
  item.head.width = static_cast<uint16_t>(w);
  item.head.height = static_cast<uint16_t>(h);
  item.head.frame_len = static_cast<uint32_t>(total);
  item.head.frame_type = 0;
  item.head.rotation = static_cast<uint8_t>(rotation);

  // Seqlock: mark slot as "done" (even seq)
  __sync_synchronize();  // write barrier: RGA data visible before seq turns even
  ctrl_->seq[w_idx]++;

  ctrl_->w_idx = (w_idx + 1) % RING_BUFFER_CNT;

  // Broadcast to ALL consumers (was pthread_cond_signal)
  pthread_cond_broadcast(&ctrl_->cv_can_read);
  pthread_mutex_unlock(&ctrl_->mtx);

  return 0;
}

#pragma clang diagnostic pop
#pragma GCC diagnostic pop
