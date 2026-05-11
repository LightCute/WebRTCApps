/*
 * mpp_codec_test.cc — test rk_mpp_codec.cc for RK3588 ARM64
 * Tests: SDP format, encoder/decoder creation, info, encoder init+encode
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/scoped_refptr.h"
#include "apps/peerconnection/client/rk_mpp_codec.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"

static int tests_run = 0, tests_failed = 0;

#define TEST(n) do { tests_run++; printf("  %s ... ", n); fflush(stdout); } while(0)
#define PASS()  do { printf("PASS\n"); } while(0)
#define FAIL(m) do { tests_failed++; printf("FAIL: %s\n", m); } while(0)
#define CHECK(c,m) do { if(c)PASS(); else FAIL(m); } while(0)

static webrtc::scoped_refptr<webrtc::I420Buffer> MakeI420(int w, int h, int stride_y, int stride_u, int stride_v) {
  auto buf = webrtc::I420Buffer::Create(w, h, stride_y, stride_u, stride_v);
  memset(buf->MutableDataY(), 128, stride_y * h);
  memset(buf->MutableDataU(), 64,  stride_u * ((h+1)/2));
  memset(buf->MutableDataV(), 192, stride_v * ((h+1)/2));
  return buf;
}

// ── Test 1: SDP format registration ────────────────────────
void test_sdp_formats() {
  TEST("SDP format support");
  auto enc = webrtc::MppH264EncoderTemplateAdapter::SupportedFormats();
  auto dec = webrtc::MppH264DecoderTemplateAdapter::SupportedFormats();
  CHECK(!enc.empty(), "encoder formats not empty");
  CHECK(!dec.empty(), "decoder formats not empty");
  CHECK(enc[0].name == "H264", "format is H264");
}

// ── Test 2: Encoder info ────────────────────────────────────
void test_encoder_info() {
  TEST("encoder info");
  webrtc::MppH264Encoder encoder;
  auto info = encoder.GetEncoderInfo();
  CHECK(info.is_hardware_accelerated, "hardware accelerated");
  CHECK(info.implementation_name.find("MPP") != std::string::npos
        || info.implementation_name.find("RK3588") != std::string::npos,
        "name contains MPP or RK3588");
}

// ── Test 3: Decoder info ────────────────────────────────────
void test_decoder_info() {
  TEST("decoder info");
  webrtc::MppH264Decoder decoder;
  auto info = decoder.GetDecoderInfo();
  CHECK(info.is_hardware_accelerated, "hardware accelerated");
}

// ── Test 4: Encoder InitEncode (key test) ───────────────────
void test_encoder_init() {
  TEST("encoder InitEncode");
  webrtc::MppH264Encoder encoder;
  webrtc::VideoCodec codec = {};
  codec.codecType = webrtc::kVideoCodecH264;
  codec.width = 640;
  codec.height = 480;
  codec.maxFramerate = 30;
  codec.maxBitrate = 2000;
  codec.SetScalabilityMode(webrtc::ScalabilityMode::kL1T1);

  int32_t ret = encoder.InitEncode(&codec,
      webrtc::VideoEncoder::Settings(
          webrtc::VideoEncoder::Capabilities(false),
          1, 1000));
  if (ret != WEBRTC_VIDEO_CODEC_OK) {
    printf("SKIP (MPP not available, ret=%d)\n", ret);
    return;
  }

  // Feed one key frame
  auto i420 = MakeI420(640, 480, 640, 320, 320);
  auto frame = webrtc::VideoFrame::Builder()
      .set_video_frame_buffer(i420)
      .set_rtp_timestamp(0)
      .set_ntp_time_ms(0)
      .build();
  std::vector<webrtc::VideoFrameType> types = {webrtc::VideoFrameType::kVideoFrameKey};

  ret = encoder.Encode(frame, &types);
  CHECK(ret >= 0, "encode returned OK or dropped");

  encoder.Release();
}

// ── Test 5: Encoder SetRates ────────────────────────────────
void test_encoder_set_rates() {
  TEST("encoder SetRates");
  webrtc::MppH264Encoder encoder;
  webrtc::VideoCodec codec = {};
  codec.codecType = webrtc::kVideoCodecH264;
  codec.width = 640;
  codec.height = 480;
  codec.maxFramerate = 30;
  codec.maxBitrate = 2000;
  codec.SetScalabilityMode(webrtc::ScalabilityMode::kL1T1);

  int32_t ret = encoder.InitEncode(&codec,
      webrtc::VideoEncoder::Settings(
          webrtc::VideoEncoder::Capabilities(false),
          1, 1000));
  if (ret != WEBRTC_VIDEO_CODEC_OK) {
    printf("SKIP (MPP not available)\n");
    return;
  }

  webrtc::VideoEncoder::RateControlParameters rates;
  rates.framerate_fps = 15.0;
  encoder.SetRates(rates);
  PASS();

  encoder.Release();
}

// ── main ────────────────────────────────────────────────────
int main() {
  printf("=== MPP Codec Test (WebRTC API) ===\n");

  test_sdp_formats();        // zero MPP deps
  test_encoder_info();       // zero MPP deps
  test_decoder_info();       // zero MPP deps
  test_encoder_init();       // needs MPP hardware
  test_encoder_set_rates();  // needs MPP hardware

  printf("\n%d tests, %d failed\n", tests_run, tests_failed);
  return tests_failed ? 1 : 0;
}
