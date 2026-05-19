// video_capture_shm_mpp — dual capture: I420 SHM + MPP H.264 SHM
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "api/environment/environment.h"
#include "api/environment/environment_factory.h"
#include "api/test/create_frame_generator.h"
#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"
#include "apps/peerconnection/client/shm_common.h"
#include "apps/peerconnection/client/shm_video_renderer.h"
#include "apps/peerconnection/video_capture_shm_mpp/enc/mpp_encoder_sink.h"
#include "common_video/libyuv/include/webrtc_libyuv.h"
#include "modules/video_capture/video_capture.h"
#include "modules/video_capture/video_capture_factory.h"
#include "rtc_base/logging.h"
#include "system_wrappers/include/clock.h"
#include "test/frame_generator_capturer.h"
#include "test/platform_video_capturer.h"
#include "test/test_video_capturer.h"

ABSL_FLAG(int, device_idx, -1, "V4L2 device index (-1 = auto-select first)");
ABSL_FLAG(std::string, shm_key, "/home/elf/webrtc_runtime/shm_video_buf",
          "Path used for ftok() key derivation (I420)");
ABSL_FLAG(int, shm_proj_id, 0x88, "Project ID for I420 SHM");
ABSL_FLAG(std::string, h264_shm_key, "/home/elf/webrtc_runtime/shm_video_buf_h264",
          "Path for H264 SHM key derivation");
ABSL_FLAG(int, h264_shm_proj_id, 0x8b, "Project ID for H264 SHM");

static std::atomic<bool> g_running{true};
static void sigint_handler(int) { g_running = false; }

namespace {

using webrtc::test::TestVideoCapturer;

std::unique_ptr<TestVideoCapturer> CreateCapturer(
    webrtc::TaskQueueFactory& task_queue_factory, int device_idx) {
  const size_t kWidth = 640;
  const size_t kHeight = 480;
  const size_t kFps = 30;

  std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> info(
      webrtc::VideoCaptureFactory::CreateDeviceInfo());
  if (info) {
    int num_devices = info->NumberOfDevices();
    if (device_idx >= 0 && device_idx < num_devices) {
      auto capturer =
          webrtc::test::CreateVideoCapturer(kWidth, kHeight, kFps, device_idx);
      if (capturer) return capturer;
    }
    for (int i = 0; i < num_devices; ++i) {
      auto capturer =
          webrtc::test::CreateVideoCapturer(kWidth, kHeight, kFps, i);
      if (capturer) return capturer;
    }
  }
  RTC_LOG(LS_WARNING)
      << "No video capture device found; using synthetic video.";
  auto frame_generator = webrtc::test::CreateSquareFrameGenerator(
      kWidth, kHeight, std::nullopt, std::nullopt);
  return std::make_unique<webrtc::test::FrameGeneratorCapturer>(
      webrtc::Clock::GetRealTimeClock(), std::move(frame_generator), kFps,
      task_queue_factory);
}

}  // namespace

int main(int argc, char* argv[]) {
  absl::ParseCommandLine(argc, argv);

  webrtc::Environment env = webrtc::CreateEnvironment();

  int device_idx = absl::GetFlag(FLAGS_device_idx);
  std::string shm_key = absl::GetFlag(FLAGS_shm_key);
  int shm_proj_id = absl::GetFlag(FLAGS_shm_proj_id);
  std::string h264_shm_key = absl::GetFlag(FLAGS_h264_shm_key);
  int h264_shm_proj_id = absl::GetFlag(FLAGS_h264_shm_proj_id);

  std::cout << "video_capture_shm_mpp starting" << std::endl;
  std::cout << "  device_idx      = " << device_idx << std::endl;
  std::cout << "  I420 SHM key    = " << shm_key << " (0x"
            << std::hex << shm_proj_id << std::dec << ")" << std::endl;
  std::cout << "  H264 SHM key    = " << h264_shm_key << " (0x"
            << std::hex << h264_shm_proj_id << std::dec << ")" << std::endl;

  // 1. Create capturer
  auto capturer = CreateCapturer(env.task_queue_factory(), device_idx);
  if (!capturer) {
    std::cerr << "Failed to create video capturer" << std::endl;
    return 1;
  }

  // 2. Create I420 SHM sink
  ShmVideoRenderer i420_sink(shm_key, shm_proj_id);

  // 3. Create MPP encoder → H264 SHM sink
  MppEncoderSink mpp_sink(env, h264_shm_key, h264_shm_proj_id);

  // 4. Register both sinks with capturer
  webrtc::VideoSinkWants wants;
  capturer->AddOrUpdateSink(&i420_sink, wants);
  capturer->AddOrUpdateSink(&mpp_sink, wants);

  // 5. Start capture
  capturer->Start();
  std::cout << "Capture started. Press Ctrl+C to stop." << std::endl;
  std::cout << "Output: I420 → " << shm_key
            << "  |  H264 → " << h264_shm_key << std::endl;

  // 6. Wait for SIGINT
  std::signal(SIGINT, sigint_handler);
  std::signal(SIGTERM, sigint_handler);
  while (g_running) {
    sleep(1);
  }

  // 7. Graceful shutdown
  std::cout << "\nShutting down..." << std::endl;
  capturer->RemoveSink(&mpp_sink);
  capturer->RemoveSink(&i420_sink);
  capturer->Stop();
  std::cout << "Done." << std::endl;
  return 0;
}
