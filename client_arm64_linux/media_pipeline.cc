#include "apps/client_arm64_linux/media_pipeline.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunsafe-buffer-usage"
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"

#include <fcntl.h>
#include <memory>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <mutex>
#include <utility>

#include "api/audio/create_audio_device_module.h"
#include "api/audio_options.h"
#include "api/make_ref_counted.h"
#include "api/task_queue/task_queue_factory.h"
#include "api/test/create_frame_generator.h"
#include "api/video/video_sink_interface.h"
#include "api/video/video_source_interface.h"
#include "apps/client_arm64_linux/shm_common.h"
#include "json/json.h"
#include "modules/video_capture/video_capture.h"
#include "modules/video_capture/video_capture_factory.h"
#include "pc/video_track_source.h"
#include "rtc_base/logging.h"
#include "system_wrappers/include/clock.h"
#include "test/frame_generator_capturer.h"
#include "test/platform_video_capturer.h"
#include "test/test_video_capturer.h"

namespace {

using webrtc::test::TestVideoCapturer;

std::unique_ptr<TestVideoCapturer> CreateCapturer(
    webrtc::TaskQueueFactory& task_queue_factory,
    int device_idx = -1) {
  const size_t kWidth = 640;
  const size_t kHeight = 480;
  const size_t kFps = 30;
  std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> info(
      webrtc::VideoCaptureFactory::CreateDeviceInfo());
  if (info) {
    int num_devices = info->NumberOfDevices();
    if (device_idx >= 0 && device_idx < num_devices) {
      std::unique_ptr<TestVideoCapturer> capturer =
          webrtc::test::CreateVideoCapturer(kWidth, kHeight, kFps, device_idx);
      if (capturer) return capturer;
    }
    for (int i = 0; i < num_devices; ++i) {
      std::unique_ptr<TestVideoCapturer> capturer =
          webrtc::test::CreateVideoCapturer(kWidth, kHeight, kFps, i);
      if (capturer) return capturer;
    }
  }
  RTC_LOG(LS_WARNING) << "No video capture device found; using synthetic video.";
  auto frame_generator = webrtc::test::CreateSquareFrameGenerator(
      kWidth, kHeight, std::nullopt, std::nullopt);
  return std::make_unique<webrtc::test::FrameGeneratorCapturer>(
      webrtc::Clock::GetRealTimeClock(), std::move(frame_generator), kFps,
      task_queue_factory);
}

class CapturerTrackSource : public webrtc::VideoTrackSource {
 public:
  static webrtc::scoped_refptr<CapturerTrackSource> Create(
      webrtc::TaskQueueFactory& task_queue_factory, int device_idx = -1) {
    std::unique_ptr<TestVideoCapturer> capturer =
        CreateCapturer(task_queue_factory, device_idx);
    if (capturer) {
      capturer->Start();
      return webrtc::make_ref_counted<CapturerTrackSource>(std::move(capturer));
    }
    return nullptr;
  }

  void SwapCapturer(std::unique_ptr<TestVideoCapturer> new_capturer) {
    if (!new_capturer) return;
    RTC_LOG(LS_INFO) << "Swapping video capturer";
    {
      std::lock_guard<std::mutex> lock(capturer_mutex_);
      capturer_->Stop();
      capturer_ = std::move(new_capturer);
      capturer_->Start();
    }
  }

 protected:
  explicit CapturerTrackSource(std::unique_ptr<TestVideoCapturer> capturer)
      : VideoTrackSource(/*remote=*/false), capturer_(std::move(capturer)) {}
  ~CapturerTrackSource() override = default;

 private:
  webrtc::VideoSourceInterface<webrtc::VideoFrame>* source() override {
    return capturer_.get();
  }
  std::mutex capturer_mutex_;
  std::unique_ptr<TestVideoCapturer> capturer_;
};

static ShmCtrlBlock* InitCtrlShmHelper(const std::string& key_path, int proj_id) {
  int fd = open(key_path.c_str(), O_CREAT | O_WRONLY, 0666);
  if (fd >= 0) close(fd);
  key_t key = ftok(key_path.c_str(), proj_id);
  if (key == -1) { perror("ftok"); return nullptr; }
  int shmid = shmget(key, SHM_CTRL_BLOCK_SIZE, IPC_CREAT | 0666);
  if (shmid == -1) { perror("shmget"); return nullptr; }
  auto* ptr = static_cast<ShmCtrlBlock*>(shmat(shmid, nullptr, 0));
  if (ptr == (void*)-1) { perror("shmat"); return nullptr; }
  if (init_shm_sync(ptr) != 0) { perror("init_shm_sync"); return nullptr; }
  return ptr;
}

static ShmMultiCtrlBlock* InitMultiCtrlShmHelper(
    const std::string& key_path, int proj_id) {
  int fd = open(key_path.c_str(), O_CREAT | O_WRONLY, 0666);
  if (fd >= 0) close(fd);
  key_t key = ftok(key_path.c_str(), proj_id);
  if (key == -1) { perror("ftok"); return nullptr; }
  int shmid = shmget(key, SHM_MULTI_CTRL_BLOCK_SIZE, IPC_CREAT | 0666);
  if (shmid == -1) { perror("shmget"); return nullptr; }
  auto* ptr = static_cast<ShmMultiCtrlBlock*>(shmat(shmid, nullptr, 0));
  if (ptr == (void*)-1) { perror("shmat"); return nullptr; }
  if (video_frame_shm_init(ptr) != 0) {
    perror("video_frame_shm_init");
    shmdt(ptr);
    return nullptr;
  }
  return ptr;
}

}  // namespace

MediaPipeline::MediaPipeline(const webrtc::Environment& env,
                             webrtc::Thread* worker_thread)
    : env_(env), worker_thread_(worker_thread) {}

MediaPipeline::~MediaPipeline() = default;

// ---- ADM ----

bool MediaPipeline::CreateAudioDeviceModule() {
  adm_ = worker_thread_->BlockingCall([this] {
    return webrtc::CreateAudioDeviceModule(
        env_, webrtc::AudioDeviceModule::kPlatformDefaultAudio);
  });
  if (!adm_) {
    RTC_LOG(LS_ERROR) << "Failed to create AudioDeviceModule";
    return false;
  }

  // Select NAU8822 for recording and playout on RK3588.
  // Must run on worker_thread since ADM ops are not thread-safe.
  worker_thread_->BlockingCall([this] {
    // Init() enables CHECKinitialized_() guards needed by
    // RecordingDevices/SetRecordingDevice/SetPlayoutDevice.
    if (adm_->Init() != 0) {
      RTC_LOG(LS_ERROR) << "ADM Init failed during device selection";
      return;
    }

    // Select recording device: find nau8822 in the enumerated names.
    int16_t rec_n = adm_->RecordingDevices();
    RTC_LOG(LS_INFO) << "Recording devices: " << rec_n;
    bool rec_found = false;
    for (int16_t i = 0; i < rec_n; i++) {
      char name[webrtc::kAdmMaxDeviceNameSize];
      char guid[webrtc::kAdmMaxGuidSize];
      if (adm_->RecordingDeviceName(i, name, guid) == 0) {
        RTC_LOG(LS_INFO) << "  RecDev[" << i << "]: " << name;
        if (!rec_found && (strstr(name, "nau8822") ||
                           strstr(name, "rockchipnau8822"))) {
          adm_->SetRecordingDevice(i);
          RTC_LOG(LS_INFO) << "  -> Selected as recording device";
          rec_found = true;
        }
      }
    }

    // Select playout device: find nau8822.
    int16_t play_n = adm_->PlayoutDevices();
    RTC_LOG(LS_INFO) << "Playout devices: " << play_n;
    bool play_found = false;
    for (int16_t i = 0; i < play_n; i++) {
      char name[webrtc::kAdmMaxDeviceNameSize];
      char guid[webrtc::kAdmMaxGuidSize];
      if (adm_->PlayoutDeviceName(i, name, guid) == 0) {
        RTC_LOG(LS_INFO) << "  PlayDev[" << i << "]: " << name;
        if (!play_found && (strstr(name, "nau8822") ||
                            strstr(name, "rockchipnau8822"))) {
          adm_->SetPlayoutDevice(i);
          RTC_LOG(LS_INFO) << "  -> Selected as playout device";
          play_found = true;
        }
      }
    }
  });

  return true;
}

// WebRtcVoiceEngine::Init() resets ADM devices back to 0.
// Call this AFTER PcFactory::Create() to re-select NAU8822 and
// re-init playout/recording with the correct ALSA cards.
void MediaPipeline::FixupAudioDeviceSelection() {
  if (!adm_) return;

  worker_thread_->BlockingCall([this] {
    // Re-enumerate and select NAU8822 (engine may have reset to device 0).
    int16_t rec_n = adm_->RecordingDevices();
    for (int16_t i = 0; i < rec_n; i++) {
      char name[webrtc::kAdmMaxDeviceNameSize];
      char guid[webrtc::kAdmMaxGuidSize];
      if (adm_->RecordingDeviceName(i, name, guid) == 0 &&
          (strstr(name, "nau8822") || strstr(name, "rockchipnau8822"))) {
        adm_->SetRecordingDevice(i);
        RTC_LOG(LS_INFO) << "Fixup: SetRecordingDevice(" << i << ") -> " << name;
        break;
      }
    }
    int16_t play_n = adm_->PlayoutDevices();
    for (int16_t i = 0; i < play_n; i++) {
      char name[webrtc::kAdmMaxDeviceNameSize];
      char guid[webrtc::kAdmMaxGuidSize];
      if (adm_->PlayoutDeviceName(i, name, guid) == 0 &&
          (strstr(name, "nau8822") || strstr(name, "rockchipnau8822"))) {
        adm_->SetPlayoutDevice(i);
        RTC_LOG(LS_INFO) << "Fixup: SetPlayoutDevice(" << i << ") -> " << name;
        break;
      }
    }

    // Re-init with the corrected devices.
    // InitPlayout/InitRecording close old handles and re-open with new indices.
    int32_t ret = adm_->InitPlayout();
    RTC_LOG(LS_INFO) << "Fixup: InitPlayout returned " << ret;
    ret = adm_->InitRecording();
    RTC_LOG(LS_INFO) << "Fixup: InitRecording returned " << ret;
  });
}

// ---- Sources ----

webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface>
MediaPipeline::CreateVideoSource() {
  video_source_ = RgaVideoTrackSource::Create(env_.task_queue_factory(),
                                               video_device_idx_);
  if (video_source_) {
    use_rga_source_ = true;
    // Allocate NV12 capture DMA-BUF pool for zero-copy encode
    const size_t kCaptureFrameSize = 640 * 480 * 3 / 2;
    capture_pool_ = std::make_unique<DmaBufPool>();
    if (capture_pool_->Allocate(kCaptureFrameSize) == 0) {
      capture_ctrl_ = InitCtrlShmHelper(shm_key_path() + "_cap",
                                         SHM_PROJ_ID + 5);
      if (capture_ctrl_) {
        auto* rga_src = static_cast<RgaVideoTrackSource*>(video_source_.get());
        rga_src->SetCapturePool(capture_pool_.get(), capture_ctrl_);
        RTC_LOG(LS_INFO) << "Capture DMA-BUF pool allocated for zero-copy encode";
      }
    }
    RTC_LOG(LS_INFO) << "Using RgaVideoTrackSource (hardware capture)";
    return video_source_;
  }
  RTC_LOG(LS_INFO) << "RGA source unavailable, using software capturer";
  video_source_ = CapturerTrackSource::Create(env_.task_queue_factory());
  return video_source_;
}

webrtc::AudioSourceInterface* MediaPipeline::CreateAudioSource(
    webrtc::PeerConnectionFactoryInterface* factory) {
  audio_source_ = factory->CreateAudioSource(webrtc::AudioOptions());
  return audio_source_.get();
}

// ---- Device management ----





// ---- Renderers ----

void MediaPipeline::StartLocalRenderer(webrtc::VideoTrackInterface* track) {
  if (use_rga_source_) {
    const size_t kFrameSize = 640 * 480 * 3 / 2;
    local_dma_pool_ = std::make_unique<DmaBufPool>();
    if (local_dma_pool_->Allocate(kFrameSize) == 0) {
      local_ctrl_ = InitMultiCtrlShmHelper(shm_video_local_key_path(),
                                           SHM_VIDEO_LOCAL_PROJ_ID);
      if (local_ctrl_) {
        int fds[DmaBufPool::kNumSlots];
        for (int i = 0; i < DmaBufPool::kNumSlots; ++i)
          fds[i] = local_dma_pool_->GetFd(i);
        local_dma_server_ = std::make_unique<DmaBufServer>();
        local_dma_server_->Start(
            shm_video_local_key_path() + "_socket",
            fds, DmaBufPool::kNumSlots,
            kFrameSize, local_ctrl_);
        auto* rga_src = static_cast<RgaVideoTrackSource*>(video_source_.get());
        rga_src->SetLocalPreview(local_dma_pool_.get(), local_ctrl_);
        RTC_LOG(LS_INFO) << "Local DMA-BUF preview started";
        return;
      }
    }
    RTC_LOG(LS_WARNING) << "DMA-BUF local preview failed, falling back to SHM";
    local_dma_pool_.reset();
  }
  if (local_renderer_) {
    RTC_LOG(LS_WARNING) << "Local SHM renderer already started";
    return;
  }
  local_renderer_ = std::make_unique<ShmVideoRenderer>(
      shm_key_path() + "_local", SHM_PROJ_ID + 1);
  track->AddOrUpdateSink(local_renderer_.get(), webrtc::VideoSinkWants());
  RTC_LOG(LS_INFO) << "Local SHM renderer started";
}

void MediaPipeline::StopLocalRenderer() {
  if (local_renderer_) {
    local_renderer_.reset();
    RTC_LOG(LS_INFO) << "Local SHM renderer stopped";
  }
}

void MediaPipeline::StartRemoteRenderer(webrtc::VideoTrackInterface* track) {
  // Try DMA-BUF hardware path first
  remote_rga_sink_ = std::make_unique<RgaDecodedSink>();
  if (remote_rga_sink_->Init()) {
    const size_t kFrameSize = 640 * 480 * 3 / 2;
    remote_dma_pool_ = std::make_unique<DmaBufPool>();
    if (remote_dma_pool_->Allocate(kFrameSize) == 0) {
      remote_ctrl_ = InitMultiCtrlShmHelper(shm_video_remote_key_path(),
                                           SHM_VIDEO_REMOTE_PROJ_ID);
      if (remote_ctrl_) {
        int fds[DmaBufPool::kNumSlots];
        for (int i = 0; i < DmaBufPool::kNumSlots; ++i)
          fds[i] = remote_dma_pool_->GetFd(i);
        remote_dma_server_ = std::make_unique<DmaBufServer>();
        remote_dma_server_->Start(
            shm_video_remote_key_path() + "_socket",
            fds, DmaBufPool::kNumSlots,
            kFrameSize, remote_ctrl_);
        remote_rga_sink_->SetOutput(remote_dma_pool_.get(), remote_ctrl_);
        webrtc::MppH264Decoder::SetDecodedHook(remote_rga_sink_.get());
        RTC_LOG(LS_INFO) << "Remote DMA-BUF sink wired to MPP decoder";
        return;
      }
    }
    RTC_LOG(LS_WARNING) << "DMA-BUF remote sink setup failed, falling back to SHM";
  }
  remote_rga_sink_.reset();
  remote_dma_pool_.reset();

  // SHM fallback
  if (remote_renderer_) {
    RTC_LOG(LS_WARNING) << "Remote SHM renderer already started";
    return;
  }
  remote_renderer_ = std::make_unique<ShmVideoRenderer>(
      shm_key_path() + "_remote", SHM_PROJ_ID + 2);
  track->AddOrUpdateSink(remote_renderer_.get(), webrtc::VideoSinkWants());
  RTC_LOG(LS_INFO) << "Remote SHM renderer started";
}

void MediaPipeline::StopRemoteRenderer() {
  webrtc::MppH264Decoder::SetDecodedHook(nullptr);
  if (remote_renderer_) {
    remote_renderer_.reset();
    RTC_LOG(LS_INFO) << "Remote SHM renderer stopped";
  }
}

// ---- Lifecycle ----

void MediaPipeline::Shutdown() {
  local_dma_server_.reset();
  remote_dma_server_.reset();
  local_dma_pool_.reset();
  remote_dma_pool_.reset();
  if (local_ctrl_) { shmdt(local_ctrl_); local_ctrl_ = nullptr; }
  if (remote_ctrl_) { shmdt(remote_ctrl_); remote_ctrl_ = nullptr; }
  if (capture_ctrl_) { shmdt(capture_ctrl_); capture_ctrl_ = nullptr; }
  capture_pool_.reset();
  remote_rga_sink_.reset();
  local_renderer_.reset();
  remote_renderer_.reset();
  video_source_ = nullptr;
  audio_source_ = nullptr;
  if (adm_ && worker_thread_) {
    auto adm = std::move(adm_);
    adm_ = nullptr;
    worker_thread_->BlockingCall([adm = std::move(adm)]() mutable {
      if (adm->Playing()) adm->StopPlayout();
      if (adm->Recording()) adm->StopRecording();
      adm = nullptr;
    });
  }
}

#pragma GCC diagnostic pop
