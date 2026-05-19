#ifndef APPS_PEERCONNECTION_VIDEO_CAPTURE_SHM_MPP_MPP_ENCODER_SINK_H_
#define APPS_PEERCONNECTION_VIDEO_CAPTURE_SHM_MPP_MPP_ENCODER_SINK_H_

#include <memory>
#include <string>

#include "api/environment/environment.h"
#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"
#include "api/video_codecs/video_encoder.h"
#include "apps/peerconnection/client/shm_video_writer.h"

// Receives raw VideoFrames from a capturer, encodes them via MPP H.264
// hardware encoder, and writes the H.264 bitstream to shared memory.
// On x86_64 (no MPP library), silently skips encoding — only I420 SHM works.
class MppEncoderSink : public webrtc::VideoSinkInterface<webrtc::VideoFrame>,
                        public webrtc::EncodedImageCallback {
 public:
  MppEncoderSink(const webrtc::Environment& env,
                 const std::string& shm_key, int proj_id);
  ~MppEncoderSink() override;

  // VideoSinkInterface — called on capturer thread
  void OnFrame(const webrtc::VideoFrame& frame) override;

 private:
  // EncodedImageCallback — called by MPP encoder (may be internal thread)
  Result OnEncodedImage(const webrtc::EncodedImage& encoded_image,
                        const webrtc::CodecSpecificInfo* codec_specific_info) override;

  bool InitEncoder(int width, int height);

  std::unique_ptr<webrtc::VideoEncoder> encoder_;
  std::unique_ptr<ShmVideoWriter> writer_;
  bool encoder_ready_ = false;
  bool logged_unavailable_ = false;

  std::string shm_key_;
  int shm_proj_id_;
  int last_width_ = 0;
  int last_height_ = 0;
};

#endif  // APPS_PEERCONNECTION_VIDEO_CAPTURE_SHM_MPP_MPP_ENCODER_SINK_H_
