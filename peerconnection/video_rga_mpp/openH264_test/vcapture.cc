// vcapture — camera or synthetic → I420 → SHM
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunsafe-buffer-usage"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"

#include <atomic>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "apps/peerconnection/client/shm_common.h"
#include "api/ref_count.h"
#include "api/scoped_refptr.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_sink_interface.h"
#include "test/platform_video_capturer.h"
#include "test/test_video_capturer.h"
#include "rtc_base/logging.h"

ABSL_FLAG(int, device_idx, -1, "V4L2 device index");

static std::atomic<bool> g_running{true};
static ShmCtrlBlock* g_shm = nullptr;
static void sigint_handler(int) {
  g_running = false;
  if (g_shm) { pthread_cond_broadcast(&g_shm->cv_can_read); }
}

static ShmCtrlBlock* InitShm(const char* path, int proj_id) {
  int fd = open(path, O_CREAT | O_WRONLY, 0666); if (fd >= 0) close(fd);
  key_t key = ftok(path, proj_id);
  if (key == -1) { perror("ftok"); return nullptr; }
  int shmid = shmget(key, SHM_CTRL_BLOCK_SIZE, IPC_CREAT | 0666);
  if (shmid == -1) { perror("shmget"); return nullptr; }
  auto* p = (ShmCtrlBlock*)shmat(shmid, nullptr, 0);
  if (p == (void*)-1) { perror("shmat"); return nullptr; }
  if (init_shm_sync(p) != 0) { perror("init_shm_sync"); return nullptr; }
  return p;
}

static void WriteSyntheticFrame(int w, int h, uint8_t r, uint8_t g, uint8_t b) {
  int ys = w * h, uvs = ys / 4;
  size_t total = ys + 2 * uvs;
  uint8_t y_val = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
  uint8_t u_val = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
  uint8_t v_val = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;

  std::vector<uint8_t> buf(total);
  memset(buf.data(), y_val, ys);
  memset(buf.data() + ys, u_val, uvs);
  memset(buf.data() + ys + uvs, v_val, uvs);

  pthread_mutex_lock(&g_shm->mtx);
  while (g_shm->frame_count >= RING_BUFFER_CNT) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 100000000;
    if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    pthread_cond_timedwait(&g_shm->cv_can_write, &g_shm->mtx, &ts);
  }
  uint32_t wi = g_shm->w_idx;
  pthread_mutex_unlock(&g_shm->mtx);

  memcpy(g_shm->ring[wi].data, buf.data(), total);
  g_shm->ring[wi].head.frame_len = total;
  g_shm->ring[wi].head.width = w; g_shm->ring[wi].head.height = h;

  pthread_mutex_lock(&g_shm->mtx);
  g_shm->w_idx = (wi + 1) % RING_BUFFER_CNT; g_shm->frame_count++;
  pthread_cond_signal(&g_shm->cv_can_read);
  pthread_mutex_unlock(&g_shm->mtx);
}

static void WriteRawI420(int w, int h,
                          const uint8_t* y, int stride_y,
                          const uint8_t* u, int stride_u,
                          const uint8_t* v, int stride_v) {
  int uv_w = (w + 1) / 2, uv_h = (h + 1) / 2;
  size_t total = (size_t)w * h + (size_t)uv_w * uv_h * 2;
  if (total > FRAME_MAX_SIZE) return;

  pthread_mutex_lock(&g_shm->mtx);
  while (g_shm->frame_count >= RING_BUFFER_CNT) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 100000000;
    if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    pthread_cond_timedwait(&g_shm->cv_can_write, &g_shm->mtx, &ts);
  }
  uint32_t wi = g_shm->w_idx;
  pthread_mutex_unlock(&g_shm->mtx);

  uint8_t* dst = g_shm->ring[wi].data;
  for (int r = 0; r < h; r++) { memcpy(dst, y + r * stride_y, w); dst += w; }
  for (int r = 0; r < uv_h; r++) { memcpy(dst, u + r * stride_u, uv_w); dst += uv_w; }
  for (int r = 0; r < uv_h; r++) { memcpy(dst, v + r * stride_v, uv_w); dst += uv_w; }

  g_shm->ring[wi].head.frame_len = total;
  g_shm->ring[wi].head.width = w; g_shm->ring[wi].head.height = h;

  pthread_mutex_lock(&g_shm->mtx);
  g_shm->w_idx = (wi + 1) % RING_BUFFER_CNT; g_shm->frame_count++;
  pthread_cond_signal(&g_shm->cv_can_read);
  pthread_mutex_unlock(&g_shm->mtx);
}

int main(int argc, char* argv[]) {
  absl::ParseCommandLine(argc, argv);
  [[maybe_unused]] const int kW = 640, kH = 480;

  std::cout << "vcapture starting" << std::endl;
  g_shm = InitShm("/tmp/webrtc_runtime/shm_vcapture", 0xb0);
  if (!g_shm) { std::cerr << "SHM init failed" << std::endl; return 1; }
  std::cout << "  SHM: /tmp/webrtc_runtime/shm_vcapture (0xb0)" << std::endl;

  // Try real camera via VcmCapturer (handles format negotiation)
  std::unique_ptr<webrtc::test::TestVideoCapturer> capturer;
  int dev_idx = absl::GetFlag(FLAGS_device_idx);
  capturer = webrtc::test::CreateVideoCapturer(640, 480, 30, dev_idx >= 0 ? dev_idx : 0);
  if (capturer) {
    capturer->Start();
    std::cout << "  camera: VcmCapturer started" << std::endl;
  } else {
    std::cout << "  camera not available, using synthetic" << std::endl;
  }

  // Sink declared here so it outlives capturer (fixes segfault on exit)
  class CamSink : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
   public:
    void OnFrame(const webrtc::VideoFrame& frame) override {
      auto i420 = frame.video_frame_buffer()->ToI420();
      if (!i420) return;
      int w = i420->width(), h = i420->height();
      WriteRawI420(w, h, i420->DataY(), i420->StrideY(),
                   i420->DataU(), i420->StrideU(),
                   i420->DataV(), i420->StrideV());
      static int fc = 0;
      if (++fc <= 5 || fc % 30 == 0) std::cout << "  captured " << fc << " frames" << std::endl;
    }
  };
  CamSink sink;

  if (capturer) {
    capturer->AddOrUpdateSink(&sink, webrtc::VideoSinkWants());
    std::cout << "Camera capture started. Ctrl+C to stop." << std::endl;
  } else {
    // Synthetic fallback thread
    std::cout << "Camera not available, using synthetic. Ctrl+C to stop." << std::endl;
    std::thread([]{
      uint8_t colors[][3] = {{255,0,0},{0,255,0},{0,0,255},{255,255,0},{255,0,255},{0,255,255}};
      int fc = 0, ci = 0;
      while (g_running) {
        WriteSyntheticFrame(640, 480, colors[ci][0], colors[ci][1], colors[ci][2]);
        if (++fc % 15 == 0) ci = (ci + 1) % 6;
        if (fc <= 5 || fc % 30 == 0) std::cout << "  generated " << fc << " frames" << std::endl;
        usleep(33333);
      }
    }).detach();
  }
  std::signal(SIGINT, sigint_handler); std::signal(SIGTERM, sigint_handler);
  while (g_running) sleep(1);

  std::cout << "\nShutting down..." << std::endl;
  g_running = false;
  if (capturer) { capturer->Stop(); }
  if (g_shm) { shmdt(g_shm); g_shm = nullptr; }
  std::cout << "Done." << std::endl; return 0;
}

#pragma clang diagnostic pop
#pragma GCC diagnostic pop
