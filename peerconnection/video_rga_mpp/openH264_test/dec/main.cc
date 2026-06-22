// openH264_test_dec — SHM H264 → OpenH264 decoder → I420 → SHM
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
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "api/environment/environment_factory.h"
#include "api/video/i420_buffer.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_decoder.h"
#include "api/video_codecs/video_decoder_factory_template_open_h264_adapter.h"
#include "apps/peerconnection/client/shm_common.h"
#include "apps/peerconnection/client/shm_video_writer.h"
#include "rtc_base/logging.h"

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

static ShmCtrlBlock* AttachReader(const std::string& key_path, int proj_id) {
  key_t key = ftok(key_path.c_str(), proj_id);
  if (key == -1) { perror("ftok"); return nullptr; }
  int shmid = shmget(key, SHM_CTRL_BLOCK_SIZE, 0666);
  if (shmid == -1) { perror("shmget"); return nullptr; }
  auto* p = (ShmCtrlBlock*)shmat(shmid, nullptr, 0);
  return (p == (void*)-1) ? nullptr : p;
}

class DecCallback : public webrtc::DecodedImageCallback {
 public:
  DecCallback(ShmVideoWriter* w) : writer_(w) {}
  int32_t Decoded(webrtc::VideoFrame& frame) override {
    if (!writer_) return 0;
    auto i420 = frame.video_frame_buffer()->ToI420();
    if (!i420) return 0;
    int w = i420->width(), h = i420->height();
    int ys = i420->StrideY() * h;
    int us = i420->StrideU() * ((h + 1) / 2);
    int vs = i420->StrideV() * ((h + 1) / 2);
    size_t total = ys + us + vs;
    if (total > FRAME_MAX_SIZE) return 0;

    std::vector<uint8_t> packed(total);
    uint8_t* dst = packed.data();
    for (int r = 0; r < h; r++) { memcpy(dst, i420->DataY() + r * i420->StrideY(), w); dst += w; }
    int uv_w = (w + 1) / 2, uv_h = (h + 1) / 2;
    for (int r = 0; r < uv_h; r++) { memcpy(dst, i420->DataU() + r * i420->StrideU(), uv_w); dst += uv_w; }
    for (int r = 0; r < uv_h; r++) { memcpy(dst, i420->DataV() + r * i420->StrideV(), uv_w); dst += uv_w; }

    VideoFrameHead head = {};
    head.frame_len = static_cast<uint32_t>(total);
    head.width = static_cast<uint16_t>(w);
    head.height = static_cast<uint16_t>(h);
    writer_->WriteFrame(head, packed.data());
    return 0;
  }
 private: ShmVideoWriter* writer_;
};

int main(int argc, char* argv[]) {
  absl::ParseCommandLine(argc, argv);
  webrtc::Environment env = webrtc::CreateEnvironment();

  std::cout << "openH264_test_dec starting" << std::endl;

  g_h264_ctrl = AttachReader("/tmp/webrtc_runtime/shm_openh264_enc", 0xa0);
  if (!g_h264_ctrl) { std::cerr << "Failed to attach H264 SHM" << std::endl; return 1; }

  ShmVideoWriter i420_writer;
  if (!i420_writer.Init("/tmp/webrtc_runtime/shm_openh264_dec", 0xa1)) {
    std::cerr << "I420 SHM init failed" << std::endl; return 1;
  }
  key_t key = ftok("/tmp/webrtc_runtime/shm_openh264_dec", 0xa1);
  int shmid = shmget(key, SHM_CTRL_BLOCK_SIZE, 0666);
  g_i420_ctrl = (ShmCtrlBlock*)shmat(shmid, nullptr, 0);

  auto decoder = webrtc::OpenH264DecoderTemplateAdapter::CreateDecoder(
      webrtc::SdpVideoFormat("H264"));
  if (!decoder) { std::cerr << "OpenH264 decoder not available" << std::endl; return 1; }
  decoder->Configure(webrtc::VideoDecoder::Settings());
  DecCallback cb(&i420_writer);
  decoder->RegisterDecodeCompleteCallback(&cb);
  std::cout << "  OpenH264 decoder ready" << std::endl;

  std::signal(SIGINT, sigint_handler); std::signal(SIGTERM, sigint_handler);
  std::vector<uint8_t> h264_buf;
  int count = 0;
  while (g_running) {
    pthread_mutex_lock(&g_h264_ctrl->mtx);
    while (g_h264_ctrl->frame_count <= 0 && g_running) {
      struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_nsec += 100000000;
      if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
      pthread_cond_timedwait(&g_h264_ctrl->cv_can_read, &g_h264_ctrl->mtx, &ts);
    }
    if (!g_running) { pthread_mutex_unlock(&g_h264_ctrl->mtx); break; }
    uint32_t ri = g_h264_ctrl->r_idx, len = g_h264_ctrl->ring[ri].head.frame_len;
    uint8_t ft = g_h264_ctrl->ring[ri].head.frame_type;
    h264_buf.resize(len);
    memcpy(h264_buf.data(), g_h264_ctrl->ring[ri].data, len);
    g_h264_ctrl->r_idx = (ri + 1) % RING_BUFFER_CNT; g_h264_ctrl->frame_count--;
    pthread_cond_signal(&g_h264_ctrl->cv_can_write);
    pthread_mutex_unlock(&g_h264_ctrl->mtx);

    webrtc::EncodedImage img;
    img.SetEncodedData(webrtc::EncodedImageBuffer::Create(h264_buf.data(), len));
    img._frameType = (ft == 1) ? webrtc::VideoFrameType::kVideoFrameKey
                               : webrtc::VideoFrameType::kVideoFrameDelta;
    decoder->Decode(img, 0); count++;
  }
  std::cout << "\nShutdown. Processed " << count << " frames." << std::endl;
  decoder->Release();
  if (g_h264_ctrl) shmdt(g_h264_ctrl);
  if (g_i420_ctrl) shmdt(g_i420_ctrl);
  return 0;
}

#pragma clang diagnostic pop
#pragma GCC diagnostic pop
