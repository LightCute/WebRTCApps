// video_rga_mpp_enc — V4L2 → RGA YUYV→NV12 → MppH264Encoder (WebRTC) → H264 DMA-BUF
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunsafe-buffer-usage"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <dlfcn.h>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <string>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "api/environment/environment_factory.h"
#include "api/scoped_refptr.h"
#include "api/video/nv12_buffer.h"
#include "api/video/video_frame.h"
#include "api/video_codecs/video_encoder.h"
#include "apps/peerconnection/client/rk_mpp_codec.h"
#include "apps/peerconnection/client/shm_common.h"
#include "apps/peerconnection/video_rga_mpp/shared/dma_buf_pool.h"
#include "apps/peerconnection/video_rga_mpp/shared/dma_buf_server.h"
#include "modules/video_capture/raw_video_sink_interface.h"
#include "modules/video_capture/video_capture.h"
#include "modules/video_capture/video_capture_factory.h"
#include "rtc_base/logging.h"

extern "C" {
#include "rga.h"
#include "drmrga.h"
}

ABSL_FLAG(int, device_idx, -1, "V4L2 device index");
ABSL_FLAG(std::string, ctrl_shm, "/home/elf/webrtc_runtime/shm_h264_ctrl",
          "Control block SHM key path");
ABSL_FLAG(int, ctrl_proj_id, 0x8e, "Control block ftok project ID");
ABSL_FLAG(std::string, socket_path, "/home/elf/webrtc_runtime/h264_socket",
          "Unix socket path for H264 dma-buf fd handoff");

static std::atomic<bool> g_running{true};
static ShmCtrlBlock* g_ctrl = nullptr;  // for cond broadcast on shutdown
static void sigint_handler(int) {
  g_running = false;
  if (g_ctrl) {
    pthread_cond_broadcast(&g_ctrl->cv_can_write);
    pthread_cond_broadcast(&g_ctrl->cv_can_read);
  }
}

static ShmCtrlBlock* InitCtrlShm(const std::string& key_path, int proj_id) {
  int fd = open(key_path.c_str(), O_CREAT | O_WRONLY, 0666);
  if (fd >= 0) close(fd);
  key_t key = ftok(key_path.c_str(), proj_id);
  if (key == -1) { perror("ftok"); return nullptr; }
  int shmid = shmget(key, SHM_CTRL_BLOCK_SIZE, IPC_CREAT | 0666);
  if (shmid == -1) { perror("shmget"); return nullptr; }
  auto* ptr = static_cast<ShmCtrlBlock*>(shmat(shmid, nullptr, 0));
  if (ptr == (void*)-1) { perror("shmat"); return nullptr; }
  if (init_shm_sync(ptr) != 0) { perror("init_shm_sync"); return nullptr; }
  std::cout << "  ctrl_shm created: " << key_path << " proj_id=0x"
            << std::hex << proj_id << std::dec << std::endl;
  return ptr;
}

// ── Encoded callback: writes H264 to DmaBufPool ──
class EncCallback : public webrtc::EncodedImageCallback {
 public:
  EncCallback(DmaBufPool* pool, ShmCtrlBlock* ctrl, int w, int h)
      : pool_(pool), ctrl_(ctrl), width_(w), height_(h) {}
  Result OnEncodedImage(const webrtc::EncodedImage& img,
                        const webrtc::CodecSpecificInfo*) override {
    if (!pool_ || !ctrl_ || !img.data() || img.size() == 0) return Result(Result::OK);
    size_t sz = pool_->GetSlotSize(0);
    if (img.size() > sz) return Result(Result::OK);

    pthread_mutex_lock(&ctrl_->mtx);
    while (ctrl_->frame_count >= RING_BUFFER_CNT) {
      struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_nsec += 100000000;
      if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
      pthread_cond_timedwait(&ctrl_->cv_can_write, &ctrl_->mtx, &ts);
    }
    uint32_t w_idx = ctrl_->w_idx;
    pthread_mutex_unlock(&ctrl_->mtx);

    memcpy(pool_->GetPtr(w_idx), img.data(), img.size());
    pthread_mutex_lock(&ctrl_->mtx);
    RingVideoFrameItem& item = ctrl_->ring[w_idx];
    memset(&item.head, 0, sizeof(item.head));
    item.head.frame_len = static_cast<uint32_t>(img.size());
    item.head.width = static_cast<uint16_t>(width_);
    item.head.height = static_cast<uint16_t>(height_);
    item.head.frame_type = (img._frameType == webrtc::VideoFrameType::kVideoFrameKey) ? 1 : 2;
    ctrl_->w_idx = (w_idx + 1) % RING_BUFFER_CNT;
    ctrl_->frame_count++;
    pthread_cond_signal(&ctrl_->cv_can_read);
    pthread_mutex_unlock(&ctrl_->mtx);
    return Result(Result::OK);
  }
 private: DmaBufPool* pool_; ShmCtrlBlock* ctrl_; int width_, height_;
};

// ── RawVideoSinkInterface: RGA YUYV→NV12 → WebRTC encoder ──
struct RgaEncoderSink : public webrtc::RawVideoSinkInterface {
  int (*rga_blit)(void*,void*,void*) = nullptr;
  std::vector<uint8_t> nv12_buf;
  webrtc::VideoEncoder* encoder = nullptr;
  int32_t OnRawFrame(uint8_t* videoFrame, size_t, const webrtc::VideoCaptureCapability& info,
                     webrtc::VideoRotation, int64_t captureTime) override {
    int w = info.width, h = info.height, ys = w * h, uvs = ys / 2;
    if (w <= 0 || h <= 0) return 0;
    if (nv12_buf.size() < (size_t)(ys + uvs)) nv12_buf.resize(ys + uvs);
    rga_info_t src{}, dst{};
    src.virAddr = videoFrame; src.format = RK_FORMAT_YUYV_422;
    src.rect.width = w; src.rect.height = h; src.rect.wstride = w; src.rect.hstride = h;
    src.rect.format = RK_FORMAT_YUYV_422; src.mmuFlag = 1; src.sync_mode = 0;
    dst.virAddr = nv12_buf.data(); dst.format = RK_FORMAT_YCbCr_420_SP;
    dst.rect.width = w; dst.rect.height = h; dst.rect.wstride = w; dst.rect.hstride = h;
    dst.rect.format = RK_FORMAT_YCbCr_420_SP; dst.mmuFlag = 1; dst.sync_mode = 0;
    if (rga_blit(&src, &dst, nullptr) != 0) return 0;
    webrtc::scoped_refptr<webrtc::NV12Buffer> nv12 = webrtc::NV12Buffer::Create(w, h);
    memcpy(nv12->MutableDataY(), nv12_buf.data(), ys);
    memcpy(nv12->MutableDataUV(), nv12_buf.data() + ys, uvs);
    encoder->Encode(webrtc::VideoFrame::Builder()
        .set_video_frame_buffer(nv12).set_timestamp_us(captureTime * 1000).build(), nullptr);
    return 0;
  }
};

int main(int argc, char* argv[]) {
  absl::ParseCommandLine(argc, argv);
  webrtc::Environment env = webrtc::CreateEnvironment();
  int device_idx = absl::GetFlag(FLAGS_device_idx);
  const int kWidth = 640, kHeight = 480, kFps = 30;
  const size_t kH264SlotSize = 256 * 1024;

  std::cout << "video_rga_mpp_enc (WebRTC MppH264Encoder) starting" << std::endl;

  // 1. VCM setup
  std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> device_info(
      webrtc::VideoCaptureFactory::CreateDeviceInfo());
  if (!device_info || device_info->NumberOfDevices() <= 0) {
    std::cerr << "No video capture devices" << std::endl; return 1;
  }
  int ndev = static_cast<int>(device_info->NumberOfDevices());
  auto try_device = [&](int idx) -> webrtc::scoped_refptr<webrtc::VideoCaptureModule> {
    char dn[256], un[256];
    if (device_info->GetDeviceName(idx, dn, sizeof(dn), un, sizeof(un)) != 0) return nullptr;
    auto v = webrtc::VideoCaptureFactory::Create(un); if (!v) return nullptr;
    webrtc::VideoCaptureCapability cap;
    cap.width = kWidth; cap.height = kHeight; cap.maxFPS = kFps;
    cap.videoType = webrtc::VideoType::kYUY2; cap.interlaced = false;
    if (v->StartCapture(cap) != 0) return nullptr;
    if (!v->CaptureStarted()) return nullptr;
    return v;
  };
  webrtc::scoped_refptr<webrtc::VideoCaptureModule> vcm;
  if (device_idx >= 0 && device_idx < ndev) vcm = try_device(device_idx);
  if (!vcm)
    for (int i = 0; i < ndev; ++i)
      if ((vcm = try_device(i))) { std::cout << "  selected device " << i << std::endl; break; }
  if (!vcm) { std::cerr << "Failed to start capture" << std::endl; return 1; }

  // 2. DMA-BUF pool + ctrl SHM + socket server
  auto h264_pool = std::make_unique<DmaBufPool>();
  if (h264_pool->Allocate(kH264SlotSize) != 0) {
    std::cerr << "H264 DmaBufPool allocation failed" << std::endl; return 1;
  }
  std::cout << "  H264 pool: " << DmaBufPool::kNumSlots << " slots x "
            << (kH264SlotSize/1024) << "KB" << std::endl;
  ShmCtrlBlock* ctrl = InitCtrlShm(absl::GetFlag(FLAGS_ctrl_shm), absl::GetFlag(FLAGS_ctrl_proj_id));
  if (!ctrl) { std::cerr << "Ctrl SHM init failed" << std::endl; return 1; }
  g_ctrl = ctrl;
  int fds[DmaBufPool::kNumSlots];
  for (int i = 0; i < DmaBufPool::kNumSlots; ++i) fds[i] = h264_pool->GetFd(i);
  DmaBufServer server;
  server.Start(absl::GetFlag(FLAGS_socket_path), fds, DmaBufPool::kNumSlots);
  std::cout << "  h264 socket listening on " << absl::GetFlag(FLAGS_socket_path) << std::endl;

  // 3. RGA + WebRTC MPP encoder
  void* rga_lib = dlopen("librga.so", RTLD_NOW | RTLD_GLOBAL);
  auto rga_blit = rga_lib ? reinterpret_cast<int(*)(void*,void*,void*)>(dlsym(rga_lib, "c_RkRgaBlit")) : nullptr;
  if (!rga_blit) { std::cerr << "RGA not available" << std::endl; return 1; }

  auto encoder = webrtc::MppH264EncoderTemplateAdapter::CreateEncoder(
      env, webrtc::SdpVideoFormat("H264"));
  if (!encoder) { std::cerr << "MPP encoder not available" << std::endl; return 1; }

  webrtc::VideoCodec codec = {};
  codec.codecType = webrtc::kVideoCodecH264; codec.width = kWidth; codec.height = kHeight;
  codec.maxFramerate = 30; codec.minBitrate = 500; codec.maxBitrate = 2000;
  codec.H264()->keyFrameInterval = 30;
  encoder->InitEncode(&codec, webrtc::VideoEncoder::Settings(
      webrtc::VideoEncoder::Capabilities(false), 1, 0));
  EncCallback cb(h264_pool.get(), ctrl, kWidth, kHeight);
  encoder->RegisterEncodeCompleteCallback(&cb);
  std::cout << "  MPP encoder ready (WebRTC VideoEncoder)" << std::endl;

  RgaEncoderSink sink;
  sink.rga_blit = rga_blit; sink.encoder = encoder.get();

  // 4. Start capture
  vcm->RegisterCaptureDataCallback(&sink);
  std::cout << "Capture at " << kWidth << "x" << kHeight << "@" << kFps
            << "fps. Ctrl+C to stop." << std::endl;

  struct sigaction sa = {};
  sa.sa_handler = sigint_handler;
  sa.sa_flags = 0;  // no SA_RESTART — allow syscalls to get EINTR
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
  while (g_running) sleep(1);

  // 5. Shutdown — broadcast cv to wake blocked threads
  std::cout << "\nShutting down..." << std::endl;
  g_running = false;
  if (ctrl) {
    pthread_mutex_lock(&ctrl->mtx);
    pthread_cond_broadcast(&ctrl->cv_can_write);
    pthread_cond_broadcast(&ctrl->cv_can_read);
    pthread_mutex_unlock(&ctrl->mtx);
  }
  vcm->StopCapture(); vcm->DeRegisterCaptureDataCallback();
  encoder->Release();
  if (ctrl) shmdt(ctrl);
  if (rga_lib) dlclose(rga_lib);
  std::cout << "Done." << std::endl; return 0;
}

#pragma clang diagnostic pop
#pragma GCC diagnostic pop
