// video_decoder_shm — read H264 from SHM, decode, write I420 to SHM
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "api/environment/environment.h"
#include "api/environment/environment_factory.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_decoder.h"
#include "apps/peerconnection/client/rk_mpp_codec.h"
#include "apps/peerconnection/client/shm_common.h"
#include "apps/peerconnection/client/shm_video_writer.h"
#include "apps/peerconnection/video_capture_shm_mpp/dec/shm_video_reader.h"
#include "rtc_base/logging.h"

ABSL_FLAG(std::string, h264_shm_key,
          "/home/elf/webrtc_runtime/shm_video_buf_h264",
          "Path for H264 SHM input");
ABSL_FLAG(int, h264_shm_proj_id, 0x8b, "Project ID for H264 SHM input");
ABSL_FLAG(std::string, decoded_shm_key,
          "/home/elf/webrtc_runtime/shm_video_buf_decoded",
          "Path for decoded I420 SHM output");
ABSL_FLAG(int, decoded_shm_proj_id, 0x8c, "Project ID for decoded I420 output");

static std::atomic<bool> g_running{true};
static void sigint_handler(int) { g_running = false; }

// Receives decoded VideoFrames and writes them to SHM.
class DecodedSink : public webrtc::DecodedImageCallback {
 public:
  DecodedSink(const std::string& shm_key, int proj_id) {
    writer_ = std::make_unique<ShmVideoWriter>();
    if (!writer_->Init(shm_key, proj_id)) {
      RTC_LOG(LS_ERROR) << "DecodedSink: ShmVideoWriter::Init failed for "
                         << shm_key;
      writer_.reset();
    }
  }

  int32_t Decoded(webrtc::VideoFrame& frame) override {
    if (!writer_) return 0;

    auto i420 = frame.video_frame_buffer()->ToI420();
    if (!i420) return 0;

    int width = i420->width();
    int height = i420->height();
    size_t y_size = i420->StrideY() * height;
    size_t u_size = i420->StrideU() * ((height + 1) / 2);
    size_t v_size = i420->StrideV() * ((height + 1) / 2);
    size_t total = y_size + u_size + v_size;

    std::vector<uint8_t> packed(total);
    uint8_t* dst = packed.data();
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunsafe-buffer-usage"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
    for (int r = 0; r < height; ++r) {
      memcpy(dst, i420->DataY() + r * i420->StrideY(), width);
      dst += width;
    }
    int half_w = (width + 1) / 2;
    int half_h = (height + 1) / 2;
    for (int r = 0; r < half_h; ++r) {
      memcpy(dst, i420->DataU() + r * i420->StrideU(), half_w);
      dst += half_w;
    }
    for (int r = 0; r < half_h; ++r) {
      memcpy(dst, i420->DataV() + r * i420->StrideV(), half_w);
      dst += half_w;
    }
#pragma clang diagnostic pop
#pragma GCC diagnostic pop

    VideoFrameHead head = {};
    head.ntp_time_ms = frame.ntp_time_ms();
    head.width = static_cast<uint16_t>(width);
    head.height = static_cast<uint16_t>(height);
    head.frame_len = static_cast<uint32_t>(total);
    head.frame_type = 0;
    head.rotation = frame.rotation();

    writer_->WriteFrame(head, packed.data());
    return 0;
  }

 private:
  std::unique_ptr<ShmVideoWriter> writer_;
};

int main(int argc, char* argv[]) {
  absl::ParseCommandLine(argc, argv);

  webrtc::Environment env = webrtc::CreateEnvironment();

  std::string h264_shm_key = absl::GetFlag(FLAGS_h264_shm_key);
  int h264_shm_proj_id = absl::GetFlag(FLAGS_h264_shm_proj_id);
  std::string decoded_shm_key = absl::GetFlag(FLAGS_decoded_shm_key);
  int decoded_shm_proj_id = absl::GetFlag(FLAGS_decoded_shm_proj_id);

  std::cout << "video_decoder_shm starting" << std::endl;
  std::cout << "  H264 SHM in   = " << h264_shm_key << " (0x"
            << std::hex << h264_shm_proj_id << std::dec << ")" << std::endl;
  std::cout << "  I420 SHM out  = " << decoded_shm_key << " (0x"
            << std::hex << decoded_shm_proj_id << std::dec << ")" << std::endl;

  // 1. Init SHM reader
  ShmVideoReader reader;
  if (!reader.Init(h264_shm_key, h264_shm_proj_id)) {
    std::cerr << "Failed to init H264 SHM reader" << std::endl;
    return 1;
  }

  // 2. Create decoder — MPP hardware first, software fallback
  std::unique_ptr<webrtc::VideoDecoder> decoder;
  auto mpp_dec = webrtc::MppH264DecoderTemplateAdapter::CreateDecoder(
      env, webrtc::SdpVideoFormat("H264"));
  if (mpp_dec) {
    decoder = std::move(mpp_dec);
    RTC_LOG(LS_INFO) << "Using MPP hardware decoder";
  } else {
    auto sw_factory = webrtc::CreateBuiltinVideoDecoderFactory();
    decoder = sw_factory->Create(env, webrtc::SdpVideoFormat("H264"));
    if (decoder) {
      RTC_LOG(LS_INFO) << "Using WebRTC builtin software H264 decoder";
    }
  }
  if (!decoder) {
    std::cerr << "Failed to create H264 decoder" << std::endl;
    return 1;
  }

  webrtc::VideoDecoder::Settings dec_settings;
  decoder->Configure(dec_settings);

  // 3. Create decoded SHM sink
  DecodedSink sink(decoded_shm_key, decoded_shm_proj_id);
  decoder->RegisterDecodeCompleteCallback(&sink);

  // 4. Main loop: read H264 → decode → write I420
  std::signal(SIGINT, sigint_handler);
  std::signal(SIGTERM, sigint_handler);

  VideoFrameHead head;
  std::vector<uint8_t> h264_data;
  int frame_count = 0;

  RTC_LOG(LS_INFO) << "Decoder loop starting, waiting for H264 frames...";
  std::cout << "Decoder loop started. Press Ctrl+C to stop." << std::endl;

  while (g_running) {
    if (!reader.ReadFrame(&head, &h264_data)) {
      if (g_running) {
        std::cerr << "ReadFrame failed" << std::endl;
      }
      break;
    }

    webrtc::EncodedImage img;
    img.SetEncodedData(
        webrtc::EncodedImageBuffer::Create(h264_data.data(), h264_data.size()));
    img._frameType = (head.frame_type == 1)
                         ? webrtc::VideoFrameType::kVideoFrameKey
                         : webrtc::VideoFrameType::kVideoFrameDelta;

    decoder->Decode(img, 0);
    frame_count++;
  }

  // 5. Cleanup
  reader.Stop();
  decoder->Release();
  RTC_LOG(LS_INFO) << "Decoder processed " << frame_count << " frames";
  std::cout << "Done. Processed " << frame_count << " frames." << std::endl;
  return 0;
}
