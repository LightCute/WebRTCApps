#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunsafe-buffer-usage"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"

#include "apps/peerconnection/video_rga_mpp/enc/rga_nv12_encoder.h"

#include <dlfcn.h>
#include <pthread.h>
#include <cstdio>
#include <cstring>

#include "apps/peerconnection/client/rk_mpp_codec.h"
#include "api/scoped_refptr.h"
#include "apps/peerconnection/client/shm_common.h"
#include "apps/peerconnection/video_rga_mpp/shared/dma_buf_pool.h"
#include "api/video/nv12_buffer.h"
#include "rtc_base/logging.h"

extern "C" {
#include "rga.h"
#include "drmrga.h"
}

RgaNv12Encoder::RgaNv12Encoder() = default;

RgaNv12Encoder::~RgaNv12Encoder() {
  if (encoder_) encoder_->Release();
  if (rga_lib_) dlclose(rga_lib_);
}

bool RgaNv12Encoder::Init(webrtc::Environment& env) {
  rga_lib_ = dlopen("librga.so", RTLD_NOW | RTLD_GLOBAL);
  if (!rga_lib_) {
    RTC_LOG(LS_WARNING) << "RgaNv12Encoder: dlopen librga.so failed";
    return false;
  }
  rga_blit_ = reinterpret_cast<int(*)(void*,void*,void*)>(
      dlsym(rga_lib_, "c_RkRgaBlit"));
  if (!rga_blit_) { dlclose(rga_lib_); rga_lib_ = nullptr; return false; }

  encoder_ = webrtc::MppH264EncoderTemplateAdapter::CreateEncoder(
      env, webrtc::SdpVideoFormat("H264"));
  if (!encoder_) {
    RTC_LOG(LS_WARNING) << "RgaNv12Encoder: MPP encoder not available";
    dlclose(rga_lib_); rga_lib_ = nullptr;
    return false;
  }
  encoder_->RegisterEncodeCompleteCallback(this);
  loaded_ = true;
  return true;
}

void RgaNv12Encoder::SetOutput(DmaBufPool* h264_pool, ShmCtrlBlock* ctrl) {
  h264_pool_ = h264_pool;
  ctrl_ = ctrl;
}

bool RgaNv12Encoder::InitEncoder(int w, int h) {
  webrtc::VideoCodec codec = {};
  codec.codecType = webrtc::kVideoCodecH264;
  codec.width = w; codec.height = h;
  codec.maxFramerate = 30;
  codec.minBitrate = 500; codec.maxBitrate = 2000;
  codec.SetFrameDropEnabled(false);
  codec.H264()->keyFrameInterval = 30;
  if (encoder_->InitEncode(&codec,
       webrtc::VideoEncoder::Settings(
           webrtc::VideoEncoder::Capabilities(false), 1, 0)) != 0) {
    RTC_LOG(LS_ERROR) << "RgaNv12Encoder: InitEncode failed";
    return false;
  }
  enc_width_ = w; enc_height_ = h;
  return true;
}

int32_t RgaNv12Encoder::OnRawFrame(uint8_t* videoFrame, size_t videoFrameLength,
                                   const webrtc::VideoCaptureCapability& frameInfo,
                                   webrtc::VideoRotation rotation, int64_t captureTime) {
  if (!loaded_ || !h264_pool_ || !ctrl_) return 0;

  int w = frameInfo.width, h = frameInfo.height;
  if (w <= 0 || h <= 0) return 0;

  if (!encoder_ || w != enc_width_ || h != enc_height_) {
    if (!InitEncoder(w, h)) return 0;
  }

  int ys = w * h, uvs = ys / 2;
  size_t nv12_size = ys + uvs;
  if (nv12_buf_.size() < nv12_size) nv12_buf_.resize(nv12_size);

  // RGA: YUYV → NV12 (DMA, zero CPU)
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

  // Build NV12 VideoFrame for MPP encoder
  webrtc::scoped_refptr<webrtc::NV12Buffer> nv12 = webrtc::NV12Buffer::Create(w, h);
  memcpy(nv12->MutableDataY(), nv12_buf_.data(), ys);
  memcpy(nv12->MutableDataUV(), nv12_buf_.data() + ys, uvs);

  webrtc::VideoFrame enc_frame = webrtc::VideoFrame::Builder()
      .set_video_frame_buffer(nv12)
      .set_rtp_timestamp(0)
      .set_timestamp_ms(captureTime)
      .set_ntp_time_ms(captureTime)
      .build();

  encoder_->Encode(enc_frame, nullptr);
  return 0;
}

webrtc::EncodedImageCallback::Result RgaNv12Encoder::OnEncodedImage(
    const webrtc::EncodedImage& encoded_image,
    const webrtc::CodecSpecificInfo*) {
  if (!h264_pool_ || !ctrl_ || !encoded_image.data() || encoded_image.size() == 0)
    return Result(Result::OK);

  pthread_mutex_lock(&ctrl_->mtx);
  while (ctrl_->frame_count >= RING_BUFFER_CNT) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 100 * 1000000;
    if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    int ret = pthread_cond_timedwait(&ctrl_->cv_can_write, &ctrl_->mtx, &ts);
    if (ret == ETIMEDOUT) { pthread_mutex_unlock(&ctrl_->mtx); return Result(Result::OK); }
  }
  uint32_t w_idx = ctrl_->w_idx;
  pthread_mutex_unlock(&ctrl_->mtx);

  size_t sz = h264_pool_->GetSlotSize(w_idx);
  if (encoded_image.size() > sz) return Result(Result::OK);

  void* dst = h264_pool_->GetPtr(w_idx);
  memcpy(dst, encoded_image.data(), encoded_image.size());

  pthread_mutex_lock(&ctrl_->mtx);
  RingVideoFrameItem& item = ctrl_->ring[w_idx];
  memset(&item.head, 0, sizeof(item.head));
  item.head.frame_len = static_cast<uint32_t>(encoded_image.size());
  item.head.width = static_cast<uint16_t>(enc_width_);
  item.head.height = static_cast<uint16_t>(enc_height_);
  item.head.frame_type = (encoded_image._frameType ==
                          webrtc::VideoFrameType::kVideoFrameKey) ? 1 : 2;
  item.head.ntp_time_ms = encoded_image.capture_time_ms_;
  ctrl_->w_idx = (w_idx + 1) % RING_BUFFER_CNT;
  ctrl_->frame_count++;
  pthread_cond_signal(&ctrl_->cv_can_read);
  pthread_mutex_unlock(&ctrl_->mtx);

  return Result(Result::OK);
}

#pragma clang diagnostic pop
#pragma GCC diagnostic pop
