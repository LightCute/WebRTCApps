/*
 * mpp_codec_debug.cc — debug version with printf stepping
 * Same test as mpp_codec_test.cc but prints before each MPP operation
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunsafe-buffer-usage"
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <memory>
#include <unistd.h>
#include <vector>

#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "apps/peerconnection/client/rk_mpp_codec.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"

// ── Mini MPP raw test at the dlopen level ──────────
static int test_dlopen_only() {
  printf("\n  [DEBUG] Testing dlopen()...\n");
  void* lib = dlopen("librockchip_mpp.so.1", RTLD_NOW);
  if (!lib) {
    printf("  dlopen FAILED: %s\n", dlerror());
    lib = dlopen("librockchip_mpp.so", RTLD_NOW);
    if (!lib) {
      printf("  dlopen .so FAILED too: %s\n", dlerror());
      return -1;
    }
  }
  printf("  dlopen OK\n");

  const char* syms[] = {
    "mpp_create", "mpp_init", "mpp_destroy",
    "mpp_buffer_group_get", "mpp_buffer_group_limit_config",
    "mpp_buffer_get_with_tag", "mpp_buffer_put_with_caller",
    "mpp_buffer_get_ptr_with_caller",
    "mpp_frame_init", "mpp_frame_deinit",
    "mpp_frame_set_width", "mpp_frame_set_height",
    "mpp_frame_set_hor_stride", "mpp_frame_set_ver_stride",
    "mpp_frame_set_buffer", "mpp_frame_set_fmt", "mpp_frame_set_eos",
    "mpp_frame_get_buffer", "mpp_frame_get_info_change",
    "mpp_frame_get_width", "mpp_frame_get_height",
    "mpp_frame_get_hor_stride", "mpp_frame_get_ver_stride",
    "mpp_packet_init", "mpp_packet_deinit", "mpp_packet_set_eos",
    "mpp_packet_get_length", "mpp_packet_get_pos", "mpp_packet_set_length",
    "mpp_enc_cfg_init", "mpp_enc_cfg_deinit", "mpp_enc_cfg_set_s32",
    NULL
  };
  for (int i = 0; syms[i]; i++) {
    void* p = dlsym(lib, syms[i]);
    if (!p) { printf("  dlsym %s FAILED\n", syms[i]); dlclose(lib); return -1; }
  }
  printf("  All 30 dlsyms OK\n");
  dlclose(lib);
  return 0;
}

// ── Test encoder with printf stepping ───────────────
static void test_encoder_debug() {
  printf("\n  [DEBUG] Creating encoder...\n"); fflush(stdout);
  webrtc::MppH264Encoder encoder;

  webrtc::VideoCodec codec = {};
  codec.codecType = webrtc::kVideoCodecH264;
  codec.width = 640;
  codec.height = 480;
  codec.maxFramerate = 30;
  codec.maxBitrate = 2000;
  codec.SetScalabilityMode(webrtc::ScalabilityMode::kL1T1);

  printf("  [DEBUG] Calling InitEncode...\n"); fflush(stdout);
  int32_t ret = encoder.InitEncode(&codec,
      webrtc::VideoEncoder::Settings(
          webrtc::VideoEncoder::Capabilities(false), 1, 1000));

  if (ret != WEBRTC_VIDEO_CODEC_OK) {
    printf("  InitEncode returned error %d — SKIP (MPP not available)\n", ret);
    return;
  }
  printf("  InitEncode OK\n"); fflush(stdout);

  // Encode one frame
  printf("  [DEBUG] Creating I420 frame...\n"); fflush(stdout);
  auto i420 = webrtc::I420Buffer::Create(640, 480);
  memset(i420->MutableDataY(), 128, i420->StrideY() * 480);
  memset(i420->MutableDataU(), 64,  i420->StrideU() * 240);
  memset(i420->MutableDataV(), 192, i420->StrideV() * 240);

  auto frame = webrtc::VideoFrame::Builder()
      .set_video_frame_buffer(i420)
      .set_rtp_timestamp(0)
      .set_ntp_time_ms(0)
      .build();

  printf("  [DEBUG] Calling Encode...\n"); fflush(stdout);
  std::vector<webrtc::VideoFrameType> types = {webrtc::VideoFrameType::kVideoFrameKey};
  ret = encoder.Encode(frame, &types);
  printf("  Encode returned %d\n", ret);

  encoder.Release();
  printf("  Encoder released OK\n");
}

// ── main ────────────────────────────────────────────
int main() {
  printf("=== MPP Debug Test ===\n");

  // Step 1: dlopen test (no WebRTC, just lib loading)
  printf("Step 1: Test dlopen + dlsym\n");
  if (test_dlopen_only() != 0) {
    printf("dlopen/dlsym failed — MPP library issue\n");
    return 1;
  }

  // Step 2: Encoder init via WebRTC adapter
  printf("\nStep 2: Test encoder via WebRTC adapter\n");
  test_encoder_debug();

  printf("\n=== Done ===\n");
  return 0;
}

#pragma GCC diagnostic pop
