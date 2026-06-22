// video_rga_mpp_dec — H264 DMA-BUF → MppH264Decoder (WebRTC) → RGA NV12→I420 → I420 DMA-BUF
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
#include "api/video/i420_buffer.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_decoder.h"
#include "apps/peerconnection/client/rk_mpp_codec.h"
#include "apps/peerconnection/client/shm_common.h"
#include "apps/peerconnection/video_rga_mpp/dec/dma_buf_client.h"
#include "apps/peerconnection/video_rga_mpp/shared/dma_buf_pool.h"
#include "apps/peerconnection/video_rga_mpp/shared/dma_buf_server.h"
#include "rtc_base/logging.h"

extern "C" {
#include "rga.h"
#include "drmrga.h"
}

ABSL_FLAG(std::string, h264_socket, "/home/elf/webrtc_runtime/h264_socket",
          "Unix socket to receive H264 dma-buf fds from enc");
ABSL_FLAG(std::string, h264_ctrl_shm, "/home/elf/webrtc_runtime/shm_h264_ctrl",
          "Control block SHM for H264 ring sync");
ABSL_FLAG(int, h264_ctrl_proj_id, 0x8e, "H264 ctrl block proj_id");
ABSL_FLAG(std::string, i420_ctrl_shm, "/home/elf/webrtc_runtime/shm_i420_ctrl",
          "Control block SHM for I420 ring sync");
ABSL_FLAG(int, i420_ctrl_proj_id, 0x8f, "I420 ctrl block proj_id");
ABSL_FLAG(std::string, i420_socket, "/home/elf/webrtc_runtime/i420_socket",
          "Unix socket to send I420 dma-buf fds to consumer");

static std::atomic<bool> g_running{true};
static ShmCtrlBlock* g_h264_ctrl = nullptr;
static ShmCtrlBlock* g_i420_ctrl = nullptr;
static void sigint_handler(int) {
  g_running = false;
  if (g_h264_ctrl) {
    pthread_cond_broadcast(&g_h264_ctrl->cv_can_read);
    pthread_cond_broadcast(&g_h264_ctrl->cv_can_write);
  }
  if (g_i420_ctrl) {
    pthread_cond_broadcast(&g_i420_ctrl->cv_can_write);
    pthread_cond_broadcast(&g_i420_ctrl->cv_can_read);
  }
}

static ShmCtrlBlock* AttachShm(const std::string& kp, int pid) {
  key_t key = ftok(kp.c_str(), pid);
  if (key == -1) { perror("ftok"); return nullptr; }
  int shmid = shmget(key, SHM_CTRL_BLOCK_SIZE, 0666);
  if (shmid == -1) { perror("shmget"); return nullptr; }
  auto* p = (ShmCtrlBlock*)shmat(shmid, nullptr, 0);
  return (p == (void*)-1) ? nullptr : p;
}

static ShmCtrlBlock* CreateShm(const std::string& kp, int pid) {
  int fd = open(kp.c_str(), O_CREAT | O_WRONLY, 0666);
  if (fd >= 0) close(fd);
  key_t key = ftok(kp.c_str(), pid);
  if (key == -1) { perror("ftok"); return nullptr; }
  int shmid = shmget(key, SHM_CTRL_BLOCK_SIZE, IPC_CREAT | 0666);
  if (shmid == -1) { perror("shmget"); return nullptr; }
  auto* p = (ShmCtrlBlock*)shmat(shmid, nullptr, 0);
  if (p == (void*)-1) { perror("shmat"); return nullptr; }
  if (init_shm_sync(p) != 0) { perror("init_shm_sync"); return nullptr; }
  return p;
}

// ── Decoded callback: RGA NV12→I420 → DmaBufPool ──
class DecSink : public webrtc::DecodedImageCallback {
 public:
  DecSink(DmaBufPool* pool, ShmCtrlBlock* ctrl, void* rga_lib,
          webrtc::MppH264Decoder* dec)
      : pool_(pool), ctrl_(ctrl), decoder_(dec) {
    rga_blit_ = reinterpret_cast<int(*)(void*,void*,void*)>(dlsym(rga_lib, "c_RkRgaBlit"));
  }
  int32_t Decoded(webrtc::VideoFrame& frame) override {
    if (!pool_ || !ctrl_ || !rga_blit_) return 0;
    int w = frame.width(), h = frame.height();
    if (w <= 0 || h <= 0) return 0;
    int ys = w * h, uvs = ys / 4; size_t total = ys + 2 * uvs;

    pthread_mutex_lock(&ctrl_->mtx);
    while (ctrl_->frame_count >= RING_BUFFER_CNT) {
      struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_nsec += 100000000;
      if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
      pthread_cond_timedwait(&ctrl_->cv_can_write, &ctrl_->mtx, &ts);
    }
    uint32_t w_idx = ctrl_->w_idx;
    pthread_mutex_unlock(&ctrl_->mtx);

    int dst_fd = pool_->GetFd(w_idx);
    rga_info_t src{}, dst{};
    // Try zero-copy: use MPP dma-buf fd as RGA source
    int mpp_fd = decoder_ ? decoder_->GetLastNV12Fd() : -1;
    if (mpp_fd >= 0) {
      // Zero-copy: RGA reads NV12 directly from MPP dma-buf fd
      src.fd = mpp_fd;
      src.format = RK_FORMAT_YCbCr_420_SP;
    } else {
      // Fallback: RGA reads NV12 from NV12Buffer CPU memory (still DMA hardware)
      auto* nv12 = frame.video_frame_buffer()->GetNV12();
      if (nv12) {
        src.virAddr = const_cast<uint8_t*>(nv12->DataY());
        src.format = RK_FORMAT_YCbCr_420_SP;
      } else {
        return 0;  // unexpected: MPP decoder always outputs NV12
      }
    }
    src.rect.width = w; src.rect.height = h; src.rect.wstride = w; src.rect.hstride = h;
    src.rect.format = src.format; src.mmuFlag = 1; src.sync_mode = 0;
    dst.fd = dst_fd; dst.format = RK_FORMAT_YCbCr_420_P;
    dst.rect.width = w; dst.rect.height = h; dst.rect.wstride = w; dst.rect.hstride = h;
    dst.rect.format = RK_FORMAT_YCbCr_420_P; dst.mmuFlag = 1; dst.sync_mode = 0;
    if (rga_blit_(&src, &dst, nullptr) != 0) return 0;

    pthread_mutex_lock(&ctrl_->mtx);
    RingVideoFrameItem& item = ctrl_->ring[w_idx];
    memset(&item.head, 0, sizeof(item.head));
    item.head.frame_len = total; item.head.width = w; item.head.height = h;
    ctrl_->w_idx = (w_idx + 1) % RING_BUFFER_CNT; ctrl_->frame_count++;
    pthread_cond_signal(&ctrl_->cv_can_read);
    pthread_mutex_unlock(&ctrl_->mtx);
    return 0;
  }
 private: DmaBufPool* pool_; ShmCtrlBlock* ctrl_;
  webrtc::MppH264Decoder* decoder_;
  int (*rga_blit_)(void*,void*,void*) = nullptr;
};

int main(int argc, char* argv[]) {
  absl::ParseCommandLine(argc, argv);
  webrtc::Environment env = webrtc::CreateEnvironment();
  const int kW = 640, kH = 480;

  std::cout << "video_rga_mpp_dec (WebRTC MppH264Decoder) starting" << std::endl;

  // 1. Receive H264 dma-buf fds from enc
  DmaBufClient client;
  auto h264_fds = client.ReceiveFds(absl::GetFlag(FLAGS_h264_socket), DmaBufPool::kNumSlots);
  if (h264_fds.size() != DmaBufPool::kNumSlots) {
    std::cerr << "Failed to receive H264 fds from enc" << std::endl; return 1;
  }
  std::cout << "  received " << h264_fds.size() << " H264 fds from enc" << std::endl;

  auto h264_pool = std::make_unique<DmaBufPool>();
  for (size_t i = 0; i < h264_fds.size(); ++i) h264_pool->ImportFd(i, h264_fds[i], 256 * 1024);

  // 2. Attach H264 ctrl SHM (created by enc)
  auto* h264_ctrl = AttachShm(absl::GetFlag(FLAGS_h264_ctrl_shm), absl::GetFlag(FLAGS_h264_ctrl_proj_id));
  if (!h264_ctrl) { std::cerr << "Failed to attach H264 ctrl SHM" << std::endl; return 1; }
  g_h264_ctrl = h264_ctrl;

  // 3. Load RGA
  void* rga_lib = dlopen("librga.so", RTLD_NOW | RTLD_GLOBAL);
  if (!rga_lib) { std::cerr << "RGA not available" << std::endl; return 1; }
  std::cout << "  RGA loaded" << std::endl;

  // 4. WebRTC MPP decoder
  auto decoder = webrtc::MppH264DecoderTemplateAdapter::CreateDecoder(
      env, webrtc::SdpVideoFormat("H264"));
  if (!decoder) { std::cerr << "MPP decoder not available" << std::endl; return 1; }
  decoder->Configure(webrtc::VideoDecoder::Settings());
  std::cout << "  MPP decoder ready (WebRTC VideoDecoder)" << std::endl;

  // 5. I420 DMA-BUF pool + ctrl SHM + socket server
  auto i420_pool = std::make_unique<DmaBufPool>();
  if (i420_pool->Allocate(kW * kH * 3 / 2) != 0) {
    std::cerr << "I420 DmaBufPool allocation failed" << std::endl; return 1;
  }
  std::cout << "  I420 pool: " << DmaBufPool::kNumSlots << " slots x "
            << (kW * kH * 3 / 2 / 1024) << "KB" << std::endl;

  auto* i420_ctrl = CreateShm(absl::GetFlag(FLAGS_i420_ctrl_shm), absl::GetFlag(FLAGS_i420_ctrl_proj_id));
  if (!i420_ctrl) { std::cerr << "I420 ctrl SHM failed" << std::endl; return 1; }
  g_i420_ctrl = i420_ctrl;

  int i420_fds[DmaBufPool::kNumSlots];
  for (int i = 0; i < DmaBufPool::kNumSlots; ++i) i420_fds[i] = i420_pool->GetFd(i);
  DmaBufServer i420_server;
  i420_server.Start(absl::GetFlag(FLAGS_i420_socket), i420_fds, DmaBufPool::kNumSlots);
  std::cout << "  i420 socket listening on " << absl::GetFlag(FLAGS_i420_socket) << std::endl;

  DecSink sink(i420_pool.get(), i420_ctrl, rga_lib,
               static_cast<webrtc::MppH264Decoder*>(decoder.get()));
  decoder->RegisterDecodeCompleteCallback(&sink);

  // 6. Main loop: read H264 from DMA-BUF, decode, output I420
  std::cout << "Decoder loop started. Ctrl+C to stop." << std::endl;
  struct sigaction sa = {};
  sa.sa_handler = sigint_handler;
  sa.sa_flags = 0;  // no SA_RESTART
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  std::vector<uint8_t> h264_buf;
  int count = 0;

  while (g_running) {
    pthread_mutex_lock(&h264_ctrl->mtx);
    while (h264_ctrl->frame_count <= 0 && g_running) {
      struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_nsec += 100000000;
      if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
      pthread_cond_timedwait(&h264_ctrl->cv_can_read, &h264_ctrl->mtx, &ts);
    }
    if (!g_running) { pthread_mutex_unlock(&h264_ctrl->mtx); break; }

    uint32_t ri = h264_ctrl->r_idx, len = h264_ctrl->ring[ri].head.frame_len;
    uint8_t ft = h264_ctrl->ring[ri].head.frame_type;
    h264_buf.resize(len);
    memcpy(h264_buf.data(), h264_pool->GetPtr(ri), len);
    h264_ctrl->r_idx = (ri + 1) % RING_BUFFER_CNT; h264_ctrl->frame_count--;
    pthread_cond_signal(&h264_ctrl->cv_can_write);
    pthread_mutex_unlock(&h264_ctrl->mtx);

    webrtc::EncodedImage img;
    img.SetEncodedData(webrtc::EncodedImageBuffer::Create(h264_buf.data(), len));
    img._frameType = (ft == 1) ? webrtc::VideoFrameType::kVideoFrameKey
                               : webrtc::VideoFrameType::kVideoFrameDelta;
    decoder->Decode(img, 0);
    count++;
  }

  std::cout << "\nShutting down. Processed " << count << " frames." << std::endl;
  g_running = false;
  if (h264_ctrl) {
    pthread_mutex_lock(&h264_ctrl->mtx);
    pthread_cond_broadcast(&h264_ctrl->cv_can_read);
    pthread_cond_broadcast(&h264_ctrl->cv_can_write);
    pthread_mutex_unlock(&h264_ctrl->mtx);
  }
  if (i420_ctrl) {
    pthread_mutex_lock(&i420_ctrl->mtx);
    pthread_cond_broadcast(&i420_ctrl->cv_can_write);
    pthread_cond_broadcast(&i420_ctrl->cv_can_read);
    pthread_mutex_unlock(&i420_ctrl->mtx);
  }
  decoder->Release();
  if (h264_ctrl) shmdt(h264_ctrl);
  if (i420_ctrl) shmdt(i420_ctrl);
  if (rga_lib) dlclose(rga_lib);
  return 0;
}

#pragma clang diagnostic pop
#pragma GCC diagnostic pop
