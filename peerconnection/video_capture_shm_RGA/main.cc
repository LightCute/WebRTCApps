// video_capture_shm_RGA — V4L2 capture → RGA hardware YUYV→I420 → dma-buf
// Falls back to libyuv software path if librga.so is unavailable.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunsafe-buffer-usage"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <string>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>


#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "api/scoped_refptr.h"
#include "api/video/video_sink_interface.h"
#include "apps/peerconnection/client/shm_common.h"
#include "apps/peerconnection/client/shm_video_renderer.h"
#include "apps/peerconnection/video_capture_shm_RGA/dma_buf_pool.h"
#include "apps/peerconnection/video_capture_shm_RGA/dma_buf_server.h"
#include "apps/peerconnection/video_capture_shm_RGA/rga_video_converter.h"
#include "modules/video_capture/video_capture.h"
#include "modules/video_capture/video_capture_factory.h"
#include "rtc_base/logging.h"

ABSL_FLAG(int, device_idx, -1, "V4L2 device index (-1 = auto-select first)");
ABSL_FLAG(std::string, shm_key, "/home/elf/webrtc_runtime/shm_video_buf",
          "Path used for ftok() ctrl-block key derivation");
ABSL_FLAG(int, shm_proj_id, 0x88, "Project ID for ftok() ctrl block");
ABSL_FLAG(std::string, socket_path, "/home/elf/webrtc_runtime/dma_buf_socket",
          "Unix socket path for dma-buf fd handoff");

static std::atomic<bool> g_running{true};
static void sigint_handler(int) { g_running = false; }

// ── Minimal SHM control block init (metadata only, frame data in dma-buf) ──
static ShmCtrlBlock* InitCtrlShm(const std::string& key_path, int proj_id) {
  // Ensure key file exists
  int fd = open(key_path.c_str(), O_CREAT | O_WRONLY, 0666);
  if (fd >= 0) close(fd);

  key_t key = ftok(key_path.c_str(), proj_id);
  if (key == -1) {
    perror("ftok ctrl");
    return nullptr;
  }

  int shmid = shmget(key, SHM_CTRL_BLOCK_SIZE, IPC_CREAT | 0666);
  if (shmid == -1) {
    perror("shmget ctrl");
    return nullptr;
  }

  auto* ptr = static_cast<ShmCtrlBlock*>(shmat(shmid, nullptr, 0));
  if (ptr == reinterpret_cast<void*>(-1)) {
    perror("shmat ctrl");
    return nullptr;
  }

  if (init_shm_sync(ptr) != 0) {
    perror("init_shm_sync ctrl");
    return nullptr;
  }
  std::cout << "  ctrl_shm created: key_path=" << key_path
            << " proj_id=0x" << std::hex << proj_id << std::dec
            << " shmid=" << shmid << std::endl;
  return ptr;
}

int main(int argc, char* argv[]) {
  absl::ParseCommandLine(argc, argv);

  int device_idx = absl::GetFlag(FLAGS_device_idx);
  std::string shm_key_path = absl::GetFlag(FLAGS_shm_key);
  int shm_proj_id = absl::GetFlag(FLAGS_shm_proj_id);
  std::string socket_path = absl::GetFlag(FLAGS_socket_path);

  const int kWidth = 640;
  const int kHeight = 480;
  const int kFps = 30;
  const size_t kFrameSize = kWidth * kHeight * 3 / 2;  // I420 packed

  std::cout << "video_capture_shm_RGA (dma-buf) starting" << std::endl;
  std::cout << "  device_idx  = " << device_idx << std::endl;
  std::cout << "  ctrl_shm    = " << shm_key_path << std::endl;
  std::cout << "  socket_path = " << socket_path << std::endl;

  // 1. Enumerate devices
  std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> device_info(
      webrtc::VideoCaptureFactory::CreateDeviceInfo());
  if (!device_info) {
    std::cerr << "Failed to create device info" << std::endl;
    return 1;
  }

  int num_devices = static_cast<int>(device_info->NumberOfDevices());
  if (num_devices <= 0) {
    std::cerr << "No video capture devices found" << std::endl;
    return 1;
  }

  auto try_device = [&](int idx) -> webrtc::scoped_refptr<webrtc::VideoCaptureModule> {
    char dev_name[256], unique_name[256];
    if (device_info->GetDeviceName(idx, dev_name, sizeof(dev_name),
                                   unique_name, sizeof(unique_name)) != 0) {
      return nullptr;
    }
    auto v = webrtc::VideoCaptureFactory::Create(unique_name);
    if (!v) return nullptr;

    webrtc::VideoCaptureCapability cap;
    cap.width = kWidth;
    cap.height = kHeight;
    cap.maxFPS = kFps;
    cap.videoType = webrtc::VideoType::kYUY2;
    cap.interlaced = false;

    if (v->StartCapture(cap) != 0) {
      cap.videoType = webrtc::VideoType::kI420;
      if (v->StartCapture(cap) != 0) return nullptr;
    }
    if (!v->CaptureStarted()) return nullptr;
    return v;
  };

  webrtc::scoped_refptr<webrtc::VideoCaptureModule> vcm;
  if (device_idx >= 0 && device_idx < num_devices) {
    vcm = try_device(device_idx);
  }
  if (!vcm) {
    for (int i = 0; i < num_devices; ++i) {
      vcm = try_device(i);
      if (vcm) {
        std::cout << "  selected device " << i << std::endl;
        break;
      }
    }
  }
  if (!vcm) {
    std::cerr << "Failed to start capture on any device" << std::endl;
    return 1;
  }

  // 2. Try RGA hardware path
  auto rga = std::make_unique<RgaVideoConverter>();
  bool use_rga = rga->Init();

  std::unique_ptr<DmaBufPool> dma_pool;
  std::unique_ptr<DmaBufServer> dma_server;
  ShmCtrlBlock* ctrl_block = nullptr;
  std::unique_ptr<ShmVideoRenderer> fallback_sink;

  if (use_rga) {
    // Allocate CMA dma-buf pool
    dma_pool = std::make_unique<DmaBufPool>();
    if (dma_pool->Allocate(kFrameSize) != 0) {
      std::cerr << "DmaBufPool allocation failed, falling back to libyuv"
                << std::endl;
      use_rga = false;
    }
  }

  if (use_rga) {
    // Init control block SHM (metadata sync, no frame data in SHM)
    ctrl_block = InitCtrlShm(shm_key_path, shm_proj_id);
    if (!ctrl_block) {
      std::cerr << "Ctrl SHM init failed, falling back to libyuv" << std::endl;
      use_rga = false;
      dma_pool.reset();
    }
  }

  if (use_rga) {
    // Start Unix socket server to hand fds to consumer
    int fd_array[DmaBufPool::kNumSlots];
    for (int i = 0; i < DmaBufPool::kNumSlots; ++i) {
      fd_array[i] = dma_pool->GetFd(i);
    }

    dma_server = std::make_unique<DmaBufServer>();
    dma_server->Start(socket_path, fd_array, DmaBufPool::kNumSlots);
    std::cout << "  dma_buf server listening on " << socket_path << std::endl;

    // Wire RGA → dma-buf pool + ctrl block
    rga->SetOutput(dma_pool.get(), ctrl_block);

    vcm->RegisterCaptureDataCallback(
        static_cast<webrtc::RawVideoSinkInterface*>(rga.get()));
    std::cout << "Using RGA hardware YUYV→I420 + DMA-BUF zero-copy" << std::endl;
  } else {
    // Fallback: software libyuv path
    std::cout << "RGA/dma-buf not available, falling back to libyuv path"
              << std::endl;
    fallback_sink = std::make_unique<ShmVideoRenderer>(shm_key_path, shm_proj_id);
    vcm->RegisterCaptureDataCallback(
        static_cast<webrtc::VideoSinkInterface<webrtc::VideoFrame>*>(
            fallback_sink.get()));
  }

  std::cout << "Capture started at " << kWidth << "x" << kHeight
            << "@" << kFps << "fps. Press Ctrl+C to stop." << std::endl;

  // 3. Wait for SIGINT
  std::signal(SIGINT, sigint_handler);
  std::signal(SIGTERM, sigint_handler);
  while (g_running) {
    sleep(1);
  }

  // 4. Shutdown
  std::cout << "\nShutting down..." << std::endl;
  vcm->StopCapture();
  vcm->DeRegisterCaptureDataCallback();
  // DmaBufPool destructor unmaps + closes fds
  // DmaBufServer destructor joins thread
  std::cout << "Done." << std::endl;
  return 0;
}

#pragma clang diagnostic pop
#pragma GCC diagnostic pop
