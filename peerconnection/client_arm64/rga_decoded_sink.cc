#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunsafe-buffer-usage"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"

#include "apps/peerconnection/client_arm64/rga_decoded_sink.h"

#include <dlfcn.h>
#include <pthread.h>
#include <cstdio>
#include <cstring>

#include "apps/peerconnection/client_arm64/dma_buf_pool.h"
#include "apps/peerconnection/client_arm64/shm_common.h"
#include "apps/peerconnection/client_arm64/rk_mpp_codec.h"
#include "rtc_base/logging.h"

extern "C" {
#include "rga.h"
#include "drmrga.h"
}

RgaDecodedSink::RgaDecodedSink() = default;

RgaDecodedSink::~RgaDecodedSink() {
  if (rga_lib_) dlclose(rga_lib_);
}

bool RgaDecodedSink::Init() {
  rga_lib_ = dlopen("librga.so", RTLD_NOW | RTLD_GLOBAL);
  if (!rga_lib_) return false;
  rga_blit_ = reinterpret_cast<int(*)(void*,void*,void*)>(
      dlsym(rga_lib_, "c_RkRgaBlit"));
  if (!rga_blit_) { dlclose(rga_lib_); rga_lib_ = nullptr; return false; }
  rga_loaded_ = true;
  return true;
}

void RgaDecodedSink::SetOutput(DmaBufPool* pool, ShmCtrlBlock* ctrl) {
  pool_ = pool;
  ctrl_ = ctrl;
}

void RgaDecodedSink::SetDecoder(webrtc::MppH264Decoder* decoder) {
  decoder_ = decoder;
}

int32_t RgaDecodedSink::Decoded(webrtc::VideoFrame& frame) {
  if (!rga_loaded_ || !pool_ || !ctrl_) return 0;

  int w = frame.width(), h = frame.height();
  if (w <= 0 || h <= 0) return 0;
  int ys = w * h, uvs = ys / 4;
  size_t total = ys + 2 * uvs;

  pthread_mutex_lock(&ctrl_->mtx);
  while (ctrl_->frame_count >= RING_BUFFER_CNT) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 100 * 1000000;
    if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    pthread_cond_timedwait(&ctrl_->cv_can_write, &ctrl_->mtx, &ts);
  }
  uint32_t w_idx = ctrl_->w_idx;
  pthread_mutex_unlock(&ctrl_->mtx);

  int dst_fd = pool_->GetFd(w_idx);

  rga_info_t src{};
  rga_info_t dst{};
  memset(&src, 0, sizeof(src));
  memset(&dst, 0, sizeof(dst));

  // Priority 1: MPP decoder dma-buf fd — zero-copy NV12 source
  int mpp_fd = decoder_ ? decoder_->GetLastNV12Fd() : -1;
  if (mpp_fd >= 0) {
    src.fd = mpp_fd;
    src.format = RK_FORMAT_YCbCr_420_SP;
  } else {
    // Priority 2: NV12 CPU buffer (MPP fd export failed or not available).
    // GetNV12() RTC_CHECKs type == kNV12 and aborts on non-NV12 frames.
    // WebRTC may adapt the frame to I420, so check the type safely first.
    auto buf = frame.video_frame_buffer();
    if (buf->type() == webrtc::VideoFrameBuffer::Type::kNV12) {
      auto* nv12 = static_cast<const webrtc::NV12BufferInterface*>(buf.get());
      src.virAddr = const_cast<uint8_t*>(nv12->DataY());
      src.format = RK_FORMAT_YCbCr_420_SP;
    } else {
      // Priority 3: I420 CPU fallback
      auto i420 = frame.video_frame_buffer()->ToI420();
      if (!i420) return 0;
      src.virAddr = const_cast<uint8_t*>(i420->DataY());
      src.format = RK_FORMAT_YCbCr_420_P;
    }
  }
  src.rect.width = w; src.rect.height = h;
  src.rect.wstride = w; src.rect.hstride = h;
  src.rect.format = src.format;
  src.mmuFlag = 1; src.sync_mode = 0;

  dst.fd = dst_fd;
  dst.format = RK_FORMAT_YCbCr_420_P;
  dst.rect.width = w; dst.rect.height = h;
  dst.rect.wstride = w; dst.rect.hstride = h;
  dst.rect.format = RK_FORMAT_YCbCr_420_P;
  dst.mmuFlag = 1; dst.sync_mode = 0;

  if (rga_blit_(&src, &dst, nullptr) != 0) {
    static int errs = 0;
    if (errs++ < 3) RTC_LOG(LS_WARNING) << "RgaDecodedSink: blit failed";
    return 0;
  }

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
