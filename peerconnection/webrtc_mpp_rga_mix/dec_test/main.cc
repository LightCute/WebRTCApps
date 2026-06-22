// dec_test — read H264 from SHM → MppH264 + OpenH264 decode → I420 → SHM
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
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "api/environment/environment_factory.h"
#include "api/video/i420_buffer.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_decoder.h"
#include "api/video_codecs/video_decoder_factory_template_open_h264_adapter.h"
#include "apps/peerconnection/client/rk_mpp_codec.h"
#include "apps/peerconnection/client/shm_common.h"
#include "rtc_base/logging.h"

ABSL_FLAG(std::string, h264_shm, "/tmp/webrtc_runtime/shm_enc_mpp",
          "H264 SHM key path (input)");
ABSL_FLAG(int, h264_proj_id, 0x93, "H264 SHM proj_id");
ABSL_FLAG(std::string, i420_mpp_shm, "/tmp/webrtc_runtime/shm_dec_mpp",
          "I420 output SHM for MPP decoder");
ABSL_FLAG(int, i420_mpp_proj_id, 0x95, "MPP decoder output proj_id");
ABSL_FLAG(std::string, i420_oh264_shm, "/tmp/webrtc_runtime/shm_dec_openh264",
          "I420 output SHM for OpenH264 decoder");
ABSL_FLAG(int, i420_oh264_proj_id, 0x96, "OpenH264 decoder output proj_id");

static std::atomic<bool> g_running{true};
static void sigint_handler(int) { g_running = false; }

static ShmCtrlBlock* InitShmWriter(const std::string& key_path, int proj_id) {
  int fd = open(key_path.c_str(), O_CREAT | O_WRONLY, 0666);
  if (fd >= 0) close(fd);
  key_t key = ftok(key_path.c_str(), proj_id);
  if (key == -1) { perror("ftok"); return nullptr; }
  int shmid = shmget(key, SHM_CTRL_BLOCK_SIZE, IPC_CREAT | 0666);
  if (shmid == -1) { perror("shmget"); return nullptr; }
  auto* ptr = (ShmCtrlBlock*)shmat(shmid, nullptr, 0);
  if (ptr == (void*)-1) { perror("shmat"); return nullptr; }
  if (init_shm_sync(ptr) != 0) { perror("init_shm_sync"); return nullptr; }
  return ptr;
}

static ShmCtrlBlock* AttachShmReader(const std::string& key_path, int proj_id) {
  key_t key = ftok(key_path.c_str(), proj_id);
  if (key == -1) { perror("ftok"); return nullptr; }
  int shmid = shmget(key, SHM_CTRL_BLOCK_SIZE, 0666);
  if (shmid == -1) { perror("shmget"); return nullptr; }
  auto* ptr = (ShmCtrlBlock*)shmat(shmid, nullptr, 0);
  if (ptr == (void*)-1) { perror("shmat"); return nullptr; }
  return ptr;
}

class DecodedI420Sink : public webrtc::DecodedImageCallback {
 public:
  DecodedI420Sink(ShmCtrlBlock* ctrl, const std::string& name)
      : ctrl_(ctrl), name_(name) {}
  int32_t Decoded(webrtc::VideoFrame& frame) override {
    if (!ctrl_) return 0;
    auto i420 = frame.video_frame_buffer()->ToI420();
    if (!i420) return 0;
    int w = i420->width(), h = i420->height();
    int ys = i420->StrideY() * h;
    int us = i420->StrideU() * ((h + 1) / 2);
    int vs = i420->StrideV() * ((h + 1) / 2);
    size_t total = ys + us + vs;
    if (total > FRAME_MAX_SIZE) return 0;

    pthread_mutex_lock(&ctrl_->mtx);
    while (ctrl_->frame_count >= RING_BUFFER_CNT) {
      struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_nsec += 100000000;
      if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
      pthread_cond_timedwait(&ctrl_->cv_can_write, &ctrl_->mtx, &ts);
    }
    uint32_t w_idx = ctrl_->w_idx;
    pthread_mutex_unlock(&ctrl_->mtx);

    uint8_t* dst = ctrl_->ring[w_idx].data;
    for (int r = 0; r < h; r++) {
      memcpy(dst, i420->DataY() + r * i420->StrideY(), w); dst += w;
    }
    int uv_w = (w + 1) / 2, uv_h = (h + 1) / 2;
    for (int r = 0; r < uv_h; r++) {
      memcpy(dst, i420->DataU() + r * i420->StrideU(), uv_w); dst += uv_w;
    }
    for (int r = 0; r < uv_h; r++) {
      memcpy(dst, i420->DataV() + r * i420->StrideV(), uv_w); dst += uv_w;
    }

    pthread_mutex_lock(&ctrl_->mtx);
    ctrl_->ring[w_idx].head.frame_len = total;
    ctrl_->ring[w_idx].head.width = w;
    ctrl_->ring[w_idx].head.height = h;
    ctrl_->ring[w_idx].head.frame_type = 0;
    ctrl_->w_idx = (w_idx + 1) % RING_BUFFER_CNT;
    ctrl_->frame_count++;
    pthread_cond_signal(&ctrl_->cv_can_read);
    pthread_mutex_unlock(&ctrl_->mtx);

    frame_count_++;
    return 0;
  }
  int frame_count() const { return frame_count_; }
 private:
  ShmCtrlBlock* ctrl_;
  std::string name_;
  int frame_count_ = 0;
};

int main(int argc, char* argv[]) {
  absl::ParseCommandLine(argc, argv);
  webrtc::Environment env = webrtc::CreateEnvironment();

  std::cout << "dec_test (MPP vs OpenH264 decoder comparison) starting" << std::endl;

  auto* h264_ctrl = AttachShmReader(absl::GetFlag(FLAGS_h264_shm),
                                     absl::GetFlag(FLAGS_h264_proj_id));
  if (!h264_ctrl) { std::cerr << "Failed to attach H264 SHM" << std::endl; return 1; }

  auto* mpp_out = InitShmWriter(absl::GetFlag(FLAGS_i420_mpp_shm),
                                 absl::GetFlag(FLAGS_i420_mpp_proj_id));
  auto* oh264_out = InitShmWriter(absl::GetFlag(FLAGS_i420_oh264_shm),
                                   absl::GetFlag(FLAGS_i420_oh264_proj_id));

  auto mpp_dec = webrtc::MppH264DecoderTemplateAdapter::CreateDecoder(
      env, webrtc::SdpVideoFormat("H264"));
  auto oh264_dec = webrtc::OpenH264DecoderTemplateAdapter::CreateDecoder(webrtc::SdpVideoFormat("H264"));

  webrtc::VideoDecoder::Settings dec_settings;
  DecodedI420Sink mpp_sink(mpp_out, "MPP"), oh264_sink(oh264_out, "OpenH264");
  if (mpp_dec) { mpp_dec->Configure(dec_settings); mpp_dec->RegisterDecodeCompleteCallback(&mpp_sink); }
  if (oh264_dec) { oh264_dec->Configure(dec_settings); oh264_dec->RegisterDecodeCompleteCallback(&oh264_sink); }

  std::cout << "  MPP decoder: " << (mpp_dec ? "ready" : "unavailable") << std::endl;
  std::cout << "  OpenH264 decoder: " << (oh264_dec ? "ready" : "unavailable") << std::endl;
  std::cout << "  Reading from: " << absl::GetFlag(FLAGS_h264_shm)
            << " (0x" << std::hex << absl::GetFlag(FLAGS_h264_proj_id) << std::dec << ")" << std::endl;
  std::cout << "  MPP I420 out → " << absl::GetFlag(FLAGS_i420_mpp_shm) << std::endl;
  std::cout << "  OpenH264 out → " << absl::GetFlag(FLAGS_i420_oh264_shm) << std::endl;

  std::signal(SIGINT, sigint_handler); std::signal(SIGTERM, sigint_handler);
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

    uint32_t r_idx = h264_ctrl->r_idx;
    uint32_t len = h264_ctrl->ring[r_idx].head.frame_len;
    h264_buf.resize(len);
    memcpy(h264_buf.data(), h264_ctrl->ring[r_idx].data, len);
    uint8_t ft = h264_ctrl->ring[r_idx].head.frame_type;
    h264_ctrl->r_idx = (r_idx + 1) % RING_BUFFER_CNT;
    h264_ctrl->frame_count--;
    pthread_cond_signal(&h264_ctrl->cv_can_write);
    pthread_mutex_unlock(&h264_ctrl->mtx);

    webrtc::EncodedImage img;
    img.SetEncodedData(webrtc::EncodedImageBuffer::Create(h264_buf.data(), len));
    img._frameType = (ft == 1) ? webrtc::VideoFrameType::kVideoFrameKey
                               : webrtc::VideoFrameType::kVideoFrameDelta;
    if (mpp_dec) mpp_dec->Decode(img, 0);
    if (oh264_dec) oh264_dec->Decode(img, 0);
    count++;
  }

  std::cout << "\nShutting down. Processed " << count << " frames." << std::endl;
  if (mpp_dec) mpp_dec->Release();
  if (oh264_dec) oh264_dec->Release();
  if (h264_ctrl) shmdt(h264_ctrl);
  if (mpp_out) shmdt(mpp_out);
  if (oh264_out) shmdt(oh264_out);
  return 0;
}

#pragma clang diagnostic pop
#pragma GCC diagnostic pop
