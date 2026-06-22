// enc_test — VCM V4L2 capture → RGA YUYV→NV12 → MppH264 + OpenH264 encode → SHM
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
#include "api/video_codecs/video_encoder_factory_template_open_h264_adapter.h"
#include "apps/peerconnection/client/rk_mpp_codec.h"
#include "apps/peerconnection/client/shm_common.h"
#include "modules/video_capture/raw_video_sink_interface.h"
#include "modules/video_capture/video_capture.h"
#include "modules/video_capture/video_capture_factory.h"
#include "rtc_base/logging.h"

extern "C" {
#include "rga.h"
#include "drmrga.h"
}

ABSL_FLAG(int, device_idx, -1, "V4L2 device index");

static std::atomic<bool> g_running{true};
static void sigint_handler(int) { g_running = false; }

// ── SHM writer ──
static ShmCtrlBlock* InitShmWriter(const std::string& key_path, int proj_id) {
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

static void WriteH264ToShm(ShmCtrlBlock* ctrl, const uint8_t* data, uint32_t len,
                           uint16_t width, uint16_t height, uint8_t frame_type) {
  pthread_mutex_lock(&ctrl->mtx);
  while (ctrl->frame_count >= RING_BUFFER_CNT) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 100 * 1000000;
    if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    pthread_cond_timedwait(&ctrl->cv_can_write, &ctrl->mtx, &ts);
  }
  uint32_t w_idx = ctrl->w_idx;
  pthread_mutex_unlock(&ctrl->mtx);

  if (len <= FRAME_MAX_SIZE) {
    memcpy(ctrl->ring[w_idx].data, data, len);
    ctrl->ring[w_idx].head.frame_len = len;
    ctrl->ring[w_idx].head.width = width;
    ctrl->ring[w_idx].head.height = height;
    ctrl->ring[w_idx].head.frame_type = frame_type;

    pthread_mutex_lock(&ctrl->mtx);
    ctrl->w_idx = (w_idx + 1) % RING_BUFFER_CNT;
    ctrl->frame_count++;
    pthread_cond_signal(&ctrl->cv_can_read);
    pthread_mutex_unlock(&ctrl->mtx);
  }
}

// ── Encoded callback → SHM ──
class H264ShmCallback : public webrtc::EncodedImageCallback {
 public:
  H264ShmCallback(ShmCtrlBlock* ctrl, int width, int height)
      : ctrl_(ctrl), width_(width), height_(height) {}
  Result OnEncodedImage(const webrtc::EncodedImage& img,
                        const webrtc::CodecSpecificInfo*) override {
    if (ctrl_ && img.data() && img.size() > 0) {
      uint8_t ft = (img._frameType == webrtc::VideoFrameType::kVideoFrameKey) ? 1 : 2;
      WriteH264ToShm(ctrl_, img.data(), img.size(), width_, height_, ft);
    }
    return Result(Result::OK);
  }
 private:
  ShmCtrlBlock* ctrl_;
  int width_, height_;
};

// ── RGA YUYV→NV12 + dual encoder ──
class DualEncoderSink : public webrtc::RawVideoSinkInterface {
 public:
  DualEncoderSink() = default;
  ~DualEncoderSink() override { if (rga_lib_) dlclose(rga_lib_); }

  bool Init(webrtc::Environment& env, int width, int height,
            ShmCtrlBlock* mpp_shm, ShmCtrlBlock* oh264_shm) {
    width_ = width; height_ = height;
    mpp_shm_ = mpp_shm; oh264_shm_ = oh264_shm;

    rga_lib_ = dlopen("librga.so", RTLD_NOW | RTLD_GLOBAL);
    if (rga_lib_)
      rga_blit_ = reinterpret_cast<int(*)(void*,void*,void*)>(dlsym(rga_lib_, "c_RkRgaBlit"));
    if (!rga_blit_) return false;

    // MPP encoder
    mpp_enc_ = webrtc::MppH264EncoderTemplateAdapter::CreateEncoder(
        env, webrtc::SdpVideoFormat("H264"));
    if (mpp_enc_) {
      webrtc::VideoCodec codec = {};
      codec.codecType = webrtc::kVideoCodecH264; codec.width = width; codec.height = height;
      codec.maxFramerate = 30; codec.minBitrate = 500; codec.maxBitrate = 2000;
      codec.H264()->keyFrameInterval = 30;
      mpp_cb_ = std::make_unique<H264ShmCallback>(mpp_shm_, width, height);
      mpp_enc_->RegisterEncodeCompleteCallback(mpp_cb_.get());
      mpp_enc_->InitEncode(&codec, webrtc::VideoEncoder::Settings(
          webrtc::VideoEncoder::Capabilities(false), 1, 0));
    }

    // OpenH264 encoder
    oh264_enc_ = webrtc::OpenH264EncoderTemplateAdapter::CreateEncoder(
        env, webrtc::SdpVideoFormat("H264"));
    if (oh264_enc_) {
      webrtc::VideoCodec codec = {};
      codec.codecType = webrtc::kVideoCodecH264; codec.width = width; codec.height = height;
      codec.maxFramerate = 30; codec.minBitrate = 500; codec.maxBitrate = 2000;
      codec.H264()->keyFrameInterval = 30;
      oh264_cb_ = std::make_unique<H264ShmCallback>(oh264_shm_, width, height);
      oh264_enc_->RegisterEncodeCompleteCallback(oh264_cb_.get());
      oh264_enc_->InitEncode(&codec, webrtc::VideoEncoder::Settings(
          webrtc::VideoEncoder::Capabilities(false), 1, 0));
    }
    return mpp_enc_ || oh264_enc_;
  }

  int32_t OnRawFrame(uint8_t* videoFrame, size_t, const webrtc::VideoCaptureCapability& info,
                     webrtc::VideoRotation, int64_t captureTime) override {
    int w = info.width, h = info.height;
    if (w <= 0 || h <= 0) return 0;
    int ys = w * h, uvs = ys / 2;

    if (nv12_buf_.size() < (size_t)(ys + uvs)) nv12_buf_.resize(ys + uvs);

    // RGA: YUYV → NV12
    if (rga_blit_) {
      rga_info_t src;
      memset(&src, 0, sizeof(src));
      src.virAddr = videoFrame; src.format = RK_FORMAT_YUYV_422;
      src.rect.width = w; src.rect.height = h;
      src.rect.wstride = w; src.rect.hstride = h;
      src.rect.format = RK_FORMAT_YUYV_422;
      src.mmuFlag = 1; src.sync_mode = 0;

      rga_info_t dst;
      memset(&dst, 0, sizeof(dst));
      dst.virAddr = nv12_buf_.data(); dst.format = RK_FORMAT_YCbCr_420_SP;
      dst.rect.width = w; dst.rect.height = h;
      dst.rect.wstride = w; dst.rect.hstride = h;
      dst.rect.format = RK_FORMAT_YCbCr_420_SP;
      dst.mmuFlag = 1; dst.sync_mode = 0;
      rga_blit_(&src, &dst, nullptr);
    }

    // Build NV12 VideoFrame for MPP (fast-path)
    webrtc::scoped_refptr<webrtc::NV12Buffer> nv12 = webrtc::NV12Buffer::Create(w, h);
    memcpy(nv12->MutableDataY(), nv12_buf_.data(), ys);
    memcpy(nv12->MutableDataUV(), nv12_buf_.data() + ys, uvs);
    webrtc::VideoFrame frame = webrtc::VideoFrame::Builder()
        .set_video_frame_buffer(nv12).set_timestamp_us(captureTime * 1000).build();

    if (mpp_enc_) mpp_enc_->Encode(frame, nullptr);

    // OpenH264: convert to I420 first
    auto i420 = frame.video_frame_buffer()->ToI420();
    if (i420 && oh264_enc_) {
      webrtc::VideoFrame i420_frame = webrtc::VideoFrame::Builder()
          .set_video_frame_buffer(i420).set_timestamp_us(captureTime * 1000).build();
      oh264_enc_->Encode(i420_frame, nullptr);
    }
    return 0;
  }

 private:
  int width_ = 640, height_ = 480;
  void* rga_lib_ = nullptr;
  int (*rga_blit_)(void*,void*,void*) = nullptr;
  std::vector<uint8_t> nv12_buf_;

  std::unique_ptr<webrtc::VideoEncoder> mpp_enc_;
  std::unique_ptr<webrtc::VideoEncoder> oh264_enc_;
  std::unique_ptr<H264ShmCallback> mpp_cb_;
  std::unique_ptr<H264ShmCallback> oh264_cb_;
  ShmCtrlBlock* mpp_shm_ = nullptr;
  ShmCtrlBlock* oh264_shm_ = nullptr;
};

int main(int argc, char* argv[]) {
  absl::ParseCommandLine(argc, argv);
  webrtc::Environment env = webrtc::CreateEnvironment();
  int device_idx = absl::GetFlag(FLAGS_device_idx);
  const int kWidth = 640, kHeight = 480;

  std::cout << "enc_test (MPP vs OpenH264 encoder comparison) starting" << std::endl;

  auto* mpp_shm = InitShmWriter("/tmp/webrtc_runtime/shm_enc_mpp", 0x93);
  auto* oh264_shm = InitShmWriter("/tmp/webrtc_runtime/shm_enc_openh264", 0x94);
  if (!mpp_shm || !oh264_shm) { std::cerr << "SHM init failed" << std::endl; return 1; }

  DualEncoderSink sink;
  if (!sink.Init(env, kWidth, kHeight, mpp_shm, oh264_shm)) {
    std::cerr << "DualEncoderSink init failed" << std::endl; return 1;
  }

  auto device_info = webrtc::VideoCaptureFactory::CreateDeviceInfo();
  if (!device_info || device_info->NumberOfDevices() <= 0) {
    std::cerr << "No capture devices" << std::endl; return 1;
  }
  int ndev = static_cast<int>(device_info->NumberOfDevices());
  int target = (device_idx >= 0 && device_idx < ndev) ? device_idx : 0;
  char name[256], uid[256];
  device_info->GetDeviceName(target, name, sizeof(name), uid, sizeof(uid));

  auto vcm = webrtc::VideoCaptureFactory::Create(uid);
  vcm->RegisterCaptureDataCallback(static_cast<webrtc::RawVideoSinkInterface*>(&sink));

  webrtc::VideoCaptureCapability cap;
  cap.width = kWidth; cap.height = kHeight; cap.maxFPS = 30;
  cap.videoType = webrtc::VideoType::kYUY2; cap.interlaced = false;
  if (vcm->StartCapture(cap) != 0) { std::cerr << "StartCapture failed" << std::endl; return 1; }

  std::cout << "Capture at 640x480@30fps. Ctrl+C to stop." << std::endl;
  std::cout << "  MPP H264  → /tmp/webrtc_runtime/shm_enc_mpp (0x93)" << std::endl;
  std::cout << "  OpenH264  → /tmp/webrtc_runtime/shm_enc_openh264 (0x94)" << std::endl;

  std::signal(SIGINT, sigint_handler);
  std::signal(SIGTERM, sigint_handler);
  while (g_running) sleep(1);

  std::cout << "\nShutting down..." << std::endl;
  vcm->StopCapture(); vcm->DeRegisterCaptureDataCallback();
  if (mpp_shm) shmdt(mpp_shm);
  if (oh264_shm) shmdt(oh264_shm);
  std::cout << "Done." << std::endl;
  return 0;
}

#pragma clang diagnostic pop
#pragma GCC diagnostic pop
