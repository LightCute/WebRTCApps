#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunsafe-buffer-usage"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"

#include "apps/peerconnection/video_rga_mpp/dec/rga_i420_decoder.h"

#include <dlfcn.h>
#include <pthread.h>
#include <cstdio>
#include <cstring>

#include "api/scoped_refptr.h"
#include "apps/peerconnection/client/shm_common.h"
#include "apps/peerconnection/video_rga_mpp/shared/dma_buf_pool.h"
#include "rtc_base/logging.h"

extern "C" {
#include "rga.h"
#include "drmrga.h"
}

RgaI420Decoder::RgaI420Decoder() = default;

RgaI420Decoder::~RgaI420Decoder() {
  if (rga_lib_) dlclose(rga_lib_);
}

bool RgaI420Decoder::Init() {
  rga_lib_ = dlopen("librga.so", RTLD_NOW | RTLD_GLOBAL);
  if (!rga_lib_) {
    RTC_LOG(LS_WARNING) << "RgaI420Decoder: dlopen librga.so failed";
    return false;
  }
  rga_blit_ = reinterpret_cast<int(*)(void*,void*,void*)>(
      dlsym(rga_lib_, "c_RkRgaBlit"));
  if (!rga_blit_) { dlclose(rga_lib_); rga_lib_ = nullptr; return false; }
  loaded_ = true;
  return true;
}

void RgaI420Decoder::SetOutput(DmaBufPool* i420_pool, ShmCtrlBlock* ctrl) {
  i420_pool_ = i420_pool;
  ctrl_ = ctrl;
}

int32_t RgaI420Decoder::Decoded(webrtc::VideoFrame& frame) {
  if (!loaded_ || !i420_pool_ || !ctrl_) return 0;

  int w = frame.width(), h = frame.height();
  if (w <= 0 || h <= 0) return 0;

  int ys = w * h, uvs = ys / 4;
  size_t total = ys + 2 * uvs;

  // MPP decoder produces I420 (CPU NV12→I420 conversion)
  auto i420 = frame.video_frame_buffer()->ToI420();
  if (!i420) return 0;

  // Wait for write slot
  pthread_mutex_lock(&ctrl_->mtx);
  while (ctrl_->frame_count >= RING_BUFFER_CNT)
    pthread_cond_wait(&ctrl_->cv_can_write, &ctrl_->mtx);
  uint32_t w_idx = ctrl_->w_idx;
  pthread_mutex_unlock(&ctrl_->mtx);

  // RGA destination: dma-buf fd (zero CPU copy to consumer)
  int dst_fd = i420_pool_->GetFd(w_idx);

  rga_info_t src;
  memset(&src, 0, sizeof(src));
  src.virAddr = const_cast<uint8_t*>(i420->DataY());
  src.format = RK_FORMAT_YCbCr_420_P;
  src.rect.width = w; src.rect.height = h;
  src.rect.wstride = w; src.rect.hstride = h;
  src.rect.format = RK_FORMAT_YCbCr_420_P;
  src.mmuFlag = 1; src.sync_mode = 0;

  rga_info_t dst;
  memset(&dst, 0, sizeof(dst));
  dst.fd = dst_fd;
  dst.format = RK_FORMAT_YCbCr_420_P;
  dst.rect.width = w; dst.rect.height = h;
  dst.rect.wstride = w; dst.rect.hstride = h;
  dst.rect.format = RK_FORMAT_YCbCr_420_P;
  dst.mmuFlag = 1; dst.sync_mode = 0;

  if (rga_blit_(&src, &dst, nullptr) != 0) {
    static int errs = 0;
    if (errs++ < 3) RTC_LOG(LS_WARNING) << "RgaI420Decoder: blit failed";
    return 0;
  }

  // Update ctrl block
  pthread_mutex_lock(&ctrl_->mtx);
  RingVideoFrameItem& item = ctrl_->ring[w_idx];
  memset(&item.head, 0, sizeof(item.head));
  item.head.frame_len = static_cast<uint32_t>(total);
  item.head.width = static_cast<uint16_t>(w);
  item.head.height = static_cast<uint16_t>(h);
  item.head.frame_type = 0;
  item.head.ntp_time_ms = frame.ntp_time_ms();
  ctrl_->w_idx = (w_idx + 1) % RING_BUFFER_CNT;
  ctrl_->frame_count++;
  pthread_cond_signal(&ctrl_->cv_can_read);
  pthread_mutex_unlock(&ctrl_->mtx);

  return 0;
}

#pragma clang diagnostic pop
#pragma GCC diagnostic pop
