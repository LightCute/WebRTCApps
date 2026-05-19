#include "apps/peerconnection/video_capture_shm_mpp/enc/mpp_encoder_sink.h"

#include "api/video/i420_buffer.h"
#include "api/video_codecs/video_encoder.h"
#include "apps/peerconnection/client/rk_mpp_codec.h"
#include "apps/peerconnection/client/shm_common.h"
#include "modules/video_coding/codecs/h264/include/h264.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "rtc_base/logging.h"

MppEncoderSink::MppEncoderSink(const webrtc::Environment& env,
                               const std::string& shm_key, int proj_id)
    : shm_key_(shm_key), shm_proj_id_(proj_id) {
  // Try loading MPP library; on x86_64 dlopen fails gracefully
  encoder_ = webrtc::MppH264EncoderTemplateAdapter::CreateEncoder(
      env, webrtc::SdpVideoFormat("H264"));
  if (encoder_) {
    writer_ = std::make_unique<ShmVideoWriter>();
    if (!writer_->Init(shm_key_, shm_proj_id_)) {
      RTC_LOG(LS_ERROR) << "MppEncoderSink: ShmVideoWriter::Init failed for "
                         << shm_key_;
      writer_.reset();
      encoder_.reset();
      return;
    }
  }
}

MppEncoderSink::~MppEncoderSink() {
  if (encoder_) {
    encoder_->Release();
  }
}

bool MppEncoderSink::InitEncoder(int width, int height) {
  webrtc::VideoCodec codec = {};
  codec.codecType = webrtc::kVideoCodecH264;
  codec.width = width;
  codec.height = height;
  codec.maxFramerate = 30;
  codec.minBitrate = 500;
  codec.maxBitrate = 2000;
  codec.SetFrameDropEnabled(false);
  codec.H264()->keyFrameInterval = 30;

  if (encoder_->InitEncode(
          &codec, webrtc::VideoEncoder::Settings(
                      webrtc::VideoEncoder::Capabilities(false), 1, 0)) != 0) {
    RTC_LOG(LS_ERROR) << "MppEncoderSink: InitEncode failed";
    return false;
  }

  encoder_->RegisterEncodeCompleteCallback(this);
  last_width_ = width;
  last_height_ = height;
  encoder_ready_ = true;
  RTC_LOG(LS_INFO) << "MppEncoderSink ready: " << width << "x" << height;
  return true;
}

void MppEncoderSink::OnFrame(const webrtc::VideoFrame& frame) {
  if (!encoder_) {
    if (!logged_unavailable_) {
      RTC_LOG(LS_INFO) << "MPP encoder unavailable; H264 SHM output disabled";
      logged_unavailable_ = true;
    }
    return;
  }

  int width = frame.width();
  int height = frame.height();

  // Reinitialize if resolution changed
  if (!encoder_ready_ || width != last_width_ || height != last_height_) {
    if (!InitEncoder(width, height)) {
      encoder_.reset();
      return;
    }
  }

  encoder_->Encode(frame, nullptr);
}

webrtc::EncodedImageCallback::Result MppEncoderSink::OnEncodedImage(
    const webrtc::EncodedImage& encoded_image,
    const webrtc::CodecSpecificInfo* /*codec_specific_info*/) {
  if (!writer_ || !encoded_image.data() || encoded_image.size() == 0) {
    return Result(Result::OK);
  }

  VideoFrameHead head = {};
  head.ntp_time_ms = encoded_image.capture_time_ms_;
  head.width = static_cast<uint16_t>(last_width_);
  head.height = static_cast<uint16_t>(last_height_);
  head.frame_len = static_cast<uint32_t>(encoded_image.size());
  head.frame_type = (encoded_image._frameType ==
                     webrtc::VideoFrameType::kVideoFrameKey)
                        ? 1
                        : 2;
  head.rotation = 0;

  writer_->WriteFrame(head, encoded_image.data());
  return Result(Result::OK);
}
