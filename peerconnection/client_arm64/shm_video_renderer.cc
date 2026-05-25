#include "apps/peerconnection/client_arm64/shm_video_renderer.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunsafe-buffer-usage"
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"

#include <cstring>

#include "common_video/libyuv/include/webrtc_libyuv.h"
#include "rtc_base/logging.h"

ShmVideoRenderer::ShmVideoRenderer(const std::string& key_path,
                                     int proj_id)
    : writer_(std::make_unique<ShmVideoWriter>()),
      io_thread_(&ShmVideoRenderer::IoLoop, this) {
  if (!writer_->Init(key_path, proj_id)) {
    RTC_LOG(LS_ERROR) << "ShmVideoRenderer: ShmVideoWriter::Init failed for "
                      << key_path;
  }
}

ShmVideoRenderer::~ShmVideoRenderer() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
  }
  cv_.notify_all();
  if (io_thread_.joinable()) {
    io_thread_.join();
  }
}

void ShmVideoRenderer::OnFrame(const webrtc::VideoFrame& frame) {
  webrtc::scoped_refptr<webrtc::I420BufferInterface> i420 =
      frame.video_frame_buffer()->ToI420();
  if (!i420) {
    RTC_LOG(LS_WARNING) << "ShmVideoRenderer::OnFrame: ToI420 returned null";
    return;
  }

  int width = i420->width();
  int height = i420->height();
  int half_width = (width + 1) / 2;
  int y_size = i420->StrideY() * height;
  int u_size = i420->StrideU() * ((height + 1) / 2);
  int v_size = i420->StrideV() * ((height + 1) / 2);
  int total_size = y_size + u_size + v_size;

  FrameData fd;
  fd.head.ntp_time_ms = frame.ntp_time_ms();
  fd.head.width = static_cast<uint16_t>(width);
  fd.head.height = static_cast<uint16_t>(height);
  fd.head.frame_type = 0;
  fd.head.rotation = frame.rotation();
  fd.head.frame_len = static_cast<uint32_t>(total_size);
  fd.i420_data.resize(total_size);

  // Copy Y plane (respecting stride)
  const uint8_t* src_y = i420->DataY();
  uint8_t* dst = fd.i420_data.data();
  for (int row = 0; row < height; ++row) {
    std::memcpy(dst, src_y, width);
    dst += width;
    src_y += i420->StrideY();
  }
  // Copy U plane (respecting stride)
  const uint8_t* src_u = i420->DataU();
  for (int row = 0; row < (height + 1) / 2; ++row) {
    std::memcpy(dst, src_u, half_width);
    dst += half_width;
    src_u += i420->StrideU();
  }
  // Copy V plane (respecting stride)
  const uint8_t* src_v = i420->DataV();
  for (int row = 0; row < (height + 1) / 2; ++row) {
    std::memcpy(dst, src_v, half_width);
    dst += half_width;
    src_v += i420->StrideV();
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_ = std::move(fd);
  }
  cv_.notify_one();
}

void ShmVideoRenderer::IoLoop() {
  while (true) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return pending_.has_value() || stopped_; });
    if (stopped_) {
      return;
    }
    if (pending_.has_value()) {
      FrameData fd = std::move(*pending_);
      pending_.reset();
      lock.unlock();
      writer_->WriteFrame(fd.head, fd.i420_data.data());
    }
  }
}

#pragma GCC diagnostic pop
