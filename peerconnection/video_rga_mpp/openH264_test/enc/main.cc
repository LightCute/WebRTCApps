// openH264_test_enc — VCM capture → OpenH264 encoder → H264 SHM output
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunsafe-buffer-usage"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"

#include <atomic>
#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <sys/ipc.h>
#include <sys/shm.h>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "api/environment/environment_factory.h"
#include "api/test/create_frame_generator.h"
#include "test/frame_generator_capturer.h"
#include "api/make_ref_counted.h"
#include "api/scoped_refptr.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"
#include "api/video/video_source_interface.h"
#include "api/video_codecs/video_encoder.h"
#include "api/video_codecs/video_encoder_factory_template_open_h264_adapter.h"
#include "apps/peerconnection/client/shm_common.h"
#include "apps/peerconnection/client/shm_video_writer.h"
#include "modules/video_capture/video_capture.h"
#include "modules/video_capture/video_capture_factory.h"
#include "pc/video_track_source.h"
#include "rtc_base/logging.h"
#include "system_wrappers/include/clock.h"
#include "test/platform_video_capturer.h"
#include "test/test_video_capturer.h"

ABSL_FLAG(int, device_idx, -1, "V4L2 device index");

static std::atomic<bool> g_running{true};
static ShmCtrlBlock* g_h264_ctrl = nullptr;
static void sigint_handler(int) {
  g_running = false;
  if (g_h264_ctrl) {
    pthread_cond_broadcast(&g_h264_ctrl->cv_can_write);
    pthread_cond_broadcast(&g_h264_ctrl->cv_can_read);
  }
}

namespace {
using webrtc::test::TestVideoCapturer;

std::unique_ptr<TestVideoCapturer> CreateCapturer(webrtc::TaskQueueFactory& tqf, int idx) {
  const size_t kW = 640, kH = 480, kFps = 30;
  auto info = webrtc::VideoCaptureFactory::CreateDeviceInfo();
  if (info) {
    int n = info->NumberOfDevices();
    if (idx >= 0 && idx < n) {
      auto c = webrtc::test::CreateVideoCapturer(kW, kH, kFps, idx);
      if (c) return c;
    }
    for (int i = 0; i < n; ++i) {
      auto c = webrtc::test::CreateVideoCapturer(kW, kH, kFps, i);
      if (c) return c;
    }
  }
  RTC_LOG(LS_WARNING) << "No capture device, using synthetic";
  auto fg = webrtc::test::CreateSquareFrameGenerator(kW, kH, std::nullopt, std::nullopt);
  return std::make_unique<webrtc::test::FrameGeneratorCapturer>(
      webrtc::Clock::GetRealTimeClock(), std::move(fg), kFps, tqf);
}

class CapturerTrackSource : public webrtc::VideoTrackSource {
 public:
  static webrtc::scoped_refptr<CapturerTrackSource> Create(
      webrtc::TaskQueueFactory& tqf, int idx) {
    auto c = CreateCapturer(tqf, idx);
    if (!c) return nullptr;
    c->Start();
    return webrtc::make_ref_counted<CapturerTrackSource>(std::move(c));
  }
 protected:
  explicit CapturerTrackSource(std::unique_ptr<TestVideoCapturer> c)
      : VideoTrackSource(false), capturer_(std::move(c)) {}
 private:
  webrtc::VideoSourceInterface<webrtc::VideoFrame>* source() override {
    return capturer_.get();
  }
  std::unique_ptr<TestVideoCapturer> capturer_;
};
}  // namespace

class EncCallback : public webrtc::EncodedImageCallback {
 public:
  EncCallback(ShmVideoWriter* w, int ww, int hh) : writer_(w), width_(ww), height_(hh) {}
  Result OnEncodedImage(const webrtc::EncodedImage& img, const webrtc::CodecSpecificInfo*) override {
    if (writer_ && img.data() && img.size() > 0) {
      VideoFrameHead head = {};
      head.frame_len = static_cast<uint32_t>(img.size());
      head.width = static_cast<uint16_t>(width_);
      head.height = static_cast<uint16_t>(height_);
      head.frame_type = (img._frameType == webrtc::VideoFrameType::kVideoFrameKey) ? 1 : 2;
      writer_->WriteFrame(head, img.data());
    }
    return Result(Result::OK);
  }
 private: ShmVideoWriter* writer_; int width_, height_;
};

int main(int argc, char* argv[]) {
  absl::ParseCommandLine(argc, argv);
  webrtc::Environment env = webrtc::CreateEnvironment();
  const int kW = 640, kH = 480;

  std::cout << "openH264_test_enc starting" << std::endl;

  ShmVideoWriter h264_writer;
  if (!h264_writer.Init("/tmp/webrtc_runtime/shm_openh264_enc", 0xa0)) {
    std::cerr << "SHM init failed" << std::endl; return 1;
  }
  key_t key = ftok("/tmp/webrtc_runtime/shm_openh264_enc", 0xa0);
  int shmid = shmget(key, SHM_CTRL_BLOCK_SIZE, 0666);
  g_h264_ctrl = (ShmCtrlBlock*)shmat(shmid, nullptr, 0);

  auto source = CapturerTrackSource::Create(env.task_queue_factory(), absl::GetFlag(FLAGS_device_idx));
  if (!source) { std::cerr << "Failed to create capturer" << std::endl; return 1; }

  auto encoder = webrtc::OpenH264EncoderTemplateAdapter::CreateEncoder(
      env, webrtc::SdpVideoFormat("H264"));
  if (!encoder) { std::cerr << "OpenH264 encoder not available" << std::endl; return 1; }
  webrtc::VideoCodec codec = {};
  codec.codecType = webrtc::kVideoCodecH264; codec.width = kW; codec.height = kH;
  codec.maxFramerate = 30; codec.minBitrate = 500; codec.maxBitrate = 2000;
  codec.H264()->keyFrameInterval = 30;
  encoder->InitEncode(&codec, webrtc::VideoEncoder::Settings(
      webrtc::VideoEncoder::Capabilities(false), 1, 0));
  EncCallback cb(&h264_writer, kW, kH);
  encoder->RegisterEncodeCompleteCallback(&cb);
  std::cout << "  OpenH264 encoder ready" << std::endl;

  class EncodeSink : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
   public:
    webrtc::VideoEncoder* enc = nullptr;
    void OnFrame(const webrtc::VideoFrame& f) override { if (enc) enc->Encode(f, nullptr); }
  } sink; sink.enc = encoder.get();
  source->AddOrUpdateSink(&sink, webrtc::VideoSinkWants());

  std::cout << "Encoding at " << kW << "x" << kH << "@30fps. Ctrl+C to stop." << std::endl;
  struct sigaction sa = {}; sa.sa_handler = sigint_handler;
  sigaction(SIGINT, &sa, nullptr); sigaction(SIGTERM, &sa, nullptr);
  while (g_running) sleep(1);

  std::cout << "\nShutting down..." << std::endl;
  g_running = false;
  if (g_h264_ctrl) {
    pthread_mutex_lock(&g_h264_ctrl->mtx);
    pthread_cond_broadcast(&g_h264_ctrl->cv_can_read);
    pthread_mutex_unlock(&g_h264_ctrl->mtx);
  }
  source->RemoveSink(&sink);
  encoder->Release();
  if (g_h264_ctrl) shmdt(g_h264_ctrl);
  std::cout << "Done." << std::endl; return 0;
}

#pragma clang diagnostic pop
#pragma GCC diagnostic pop
