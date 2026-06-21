// rk_mpp_encoder.cc — RK3588 MPP H264 encoder (dlopen, no link-time dep)
#include "apps/client_arm64_linux/rk_mpp_encoder.h"

#include <dlfcn.h>
#include <cstring>
#include <vector>

#include "api/video_codecs/video_encoder.h"
#include "api/video/i420_buffer.h"
#include "modules/video_coding/codecs/h264/include/h264.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"

// ── MPP types (self-contained, no system header deps) ──────
typedef int32_t MPP_RET;
#define MPP_OK 0
typedef void* MppCtx;
typedef struct { void* opaque; }* MppApi;
typedef void* MppBufferGroup;
typedef void* MppBuffer;
typedef void* MppFrame;
typedef void* MppPacket;
typedef uint32_t RK_U32;
typedef uint32_t MppCodingType;
typedef uint32_t MppFrameFormat;
typedef uint32_t MppPollType;
typedef uint32_t MppEncRcMode;
typedef uint32_t MppEncRcQuality;

enum { MPP_VIDEO_CodingAVC = 7, MPP_CTX_ENC = 0, MPP_POLL_BLOCK = 0 };
enum { MPP_BUFFER_TYPE_DRM = 0, MPP_BUFFER_TYPE_ION = 1 };
enum { MPP_FMT_YUV420SP = 0x00000100 };
enum { MPP_ENC_RC_MODE_CBR = 1, MPP_ENC_RC_QUALITY_MEDIUM = 2 };

enum MppEncCfgChange {
  MPP_ENC_H264_CFG_CHANGE_PROFILE = (1<<1),
  MPP_ENC_PREP_CFG_CHANGE_INPUT = (1<<0),
  MPP_ENC_RC_CFG_CHANGE_ALL = 0xFFFFFFFF,
  MPP_ENC_RC_CFG_CHANGE_BPS = (1<<0),
};

struct MppEncCodecCfg_t { RK_U32 change; MppCodingType coding; struct { RK_U32 change; int32_t profile, level; } h264; };
struct MppEncPrepCfg_t { RK_U32 change; RK_U32 width, height, hor_stride, ver_stride; MppFrameFormat format; };
struct MppEncRcCfg_t { RK_U32 change; MppEncRcMode rc_mode; MppEncRcQuality quality;
  int bps_target, bps_max, bps_min; int fps_in_flex, fps_in_num, fps_in_denom;
  int fps_out_flex, fps_out_num, fps_out_denom; int gop; };

enum MppEncCfg { MPP_ENC_SET_CODEC_CFG=0, MPP_ENC_SET_PREP_CFG=1, MPP_ENC_SET_RC_CFG=2 };
enum MppCfg { MPP_SET_OUTPUT_TIMEOUT=0 };

// ── Function pointer types ─────────────────────────────────
typedef MPP_RET (*MppCreateFn)(MppCtx*, MppApi*);
typedef MPP_RET (*MppInitFn)(MppCtx, MppCodingType, MppCodingType);
typedef MPP_RET (*MppDestroyFn)(MppCtx);
typedef MPP_RET (*MppCtrlFn)(MppCtx, uint32_t, void*);
typedef MPP_RET (*MppBufGroupGetFn)(MppBufferGroup*, uint32_t);
typedef MPP_RET (*MppBufGroupLimitFn)(MppBufferGroup, size_t, int);
typedef MPP_RET (*MppBufGetFn)(MppBufferGroup, MppBuffer*, size_t);
typedef MPP_RET (*MppBufPutFn)(MppBuffer);
typedef void* (*MppBufGetPtrFn)(MppBuffer);
typedef MPP_RET (*MppFrmInitFn)(MppFrame*);
typedef MPP_RET (*MppFrmDeinitFn)(MppFrame);
typedef MPP_RET (*MppFrmSetIntFn)(MppFrame, int);
typedef MPP_RET (*MppFrmSetBufFn)(MppFrame, MppBuffer);
typedef MPP_RET (*MppFrmSetFmtFn)(MppFrame, MppFrameFormat);
typedef MPP_RET (*MppPktDeinitFn)(MppPacket);
typedef size_t (*MppPktGetLenFn)(MppPacket);
typedef void* (*MppPktGetDataFn)(MppPacket);
typedef MPP_RET (*MppEncPutFrameFn)(MppCtx, MppApi*, MppFrame);
typedef MPP_RET (*MppEncGetPacketFn)(MppCtx, MppApi*, MppPacket*);

static struct {
  void* lib = nullptr;
  MppCreateFn mpp_create;
  MppInitFn mpp_init;
  MppDestroyFn mpp_destroy;
  MppCtrlFn control;
  MppBufGroupGetFn mpp_buffer_group_get_internal;
  MppBufGroupLimitFn mpp_buffer_group_limit_config;
  MppBufGetFn mpp_buffer_get;
  MppBufPutFn mpp_buffer_put;
  MppBufPutFn mpp_buffer_group_put;
  MppBufGetPtrFn mpp_buffer_get_ptr;
  MppFrmInitFn mpp_frame_init;
  MppFrmDeinitFn mpp_frame_deinit;
  MppFrmSetIntFn mpp_frame_set_width, mpp_frame_set_height, mpp_frame_set_hor_stride, mpp_frame_set_ver_stride;
  MppFrmSetBufFn mpp_frame_set_buffer;
  MppFrmSetFmtFn mpp_frame_set_fmt;
  MppPktDeinitFn mpp_packet_deinit;
  MppPktGetLenFn mpp_packet_get_length;
  MppPktGetDataFn mpp_packet_get_data;
  MppEncPutFrameFn encode_put_frame;
  MppEncGetPacketFn encode_get_packet;
} mpp;

#define LOAD(fn) mpp.fn = (decltype(mpp.fn))dlsym(mpp.lib, #fn)
static bool MppLoad() {
  if (mpp.lib) return true;
  mpp.lib = dlopen("librockchip_mpp.so.1", RTLD_NOW);
  if (!mpp.lib) { RTC_LOG(LS_WARNING) << "MPP not available, using software H264"; return false; }
  LOAD(mpp_create); LOAD(mpp_init); LOAD(mpp_destroy); LOAD(control);
  LOAD(mpp_buffer_group_get_internal); LOAD(mpp_buffer_group_limit_config);
  LOAD(mpp_buffer_get); LOAD(mpp_buffer_put); LOAD(mpp_buffer_group_put); LOAD(mpp_buffer_get_ptr);
  LOAD(mpp_frame_init); LOAD(mpp_frame_deinit);
  LOAD(mpp_frame_set_width); LOAD(mpp_frame_set_height);
  LOAD(mpp_frame_set_hor_stride); LOAD(mpp_frame_set_ver_stride);
  LOAD(mpp_frame_set_buffer); LOAD(mpp_frame_set_fmt);
  LOAD(mpp_packet_deinit); LOAD(mpp_packet_get_length); LOAD(mpp_packet_get_data);
  LOAD(encode_put_frame); LOAD(encode_get_packet);
  RTC_LOG(LS_INFO) << "MPP loaded successfully";
  return true;
}
#undef LOAD

#define ALIGN(x,a) (((x)+(a)-1)&~((a)-1))

namespace webrtc {

std::vector<SdpVideoFormat> MppH264EncoderTemplateAdapter::SupportedFormats() {
  return {CreateH264Format(H264Profile::kProfileBaseline, H264Level::kLevel3_1, "1", false),
          CreateH264Format(H264Profile::kProfileBaseline, H264Level::kLevel3_1, "0", false),
          CreateH264Format(H264Profile::kProfileConstrainedBaseline, H264Level::kLevel3_1, "1", false)};
}

std::unique_ptr<VideoEncoder> MppH264EncoderTemplateAdapter::CreateEncoder(
    const Environment&, const SdpVideoFormat&) {
  if (!MppLoad()) return nullptr;  // fallback to next adapter
  return std::make_unique<MppH264Encoder>();
}

struct MppH264Encoder::Impl {
  MppCtx ctx = nullptr;
  MppApi mpi = nullptr;
  MppBufferGroup buf_grp = nullptr;
  MppBuffer frm_buf = nullptr;
  EncodedImageCallback* callback = nullptr;
  VideoCodec codec_config = {};
  size_t frame_size = 0;
  int w = 0, h = 0;
  bool ready = false;

  ~Impl() { Release(); }

  bool InitEncoder() {
    if (mpp.mpp_create(&ctx, &mpi) != MPP_OK) return false;
    if (mpp.mpp_init(ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC) != MPP_OK) return false;
    w = codec_config.width; h = codec_config.height;
    frame_size = w * ALIGN(h, 16) * 3 / 2;

    if (mpp.mpp_buffer_group_get_internal(&buf_grp, MPP_BUFFER_TYPE_DRM) != MPP_OK)
      mpp.mpp_buffer_group_get_internal(&buf_grp, MPP_BUFFER_TYPE_ION);
    if (!buf_grp) return false;
    mpp.mpp_buffer_group_limit_config(buf_grp, frame_size, 4);
    if (mpp.mpp_buffer_get(buf_grp, &frm_buf, frame_size) != MPP_OK) return false;

    MppEncCodecCfg_t cc = {}; cc.coding = MPP_VIDEO_CodingAVC;
    cc.h264.change = MPP_ENC_H264_CFG_CHANGE_PROFILE; cc.h264.profile = 100; cc.h264.level = 42;
    mpp.control(ctx, MPP_ENC_SET_CODEC_CFG, &cc);

    MppEncPrepCfg_t pc = {}; pc.change = MPP_ENC_PREP_CFG_CHANGE_INPUT;
    pc.width = w; pc.height = h; pc.hor_stride = w; pc.ver_stride = ALIGN(h, 16);
    pc.format = MPP_FMT_YUV420SP;
    mpp.control(ctx, MPP_ENC_SET_PREP_CFG, &pc);

    MppEncRcCfg_t rc = {}; rc.change = MPP_ENC_RC_CFG_CHANGE_ALL;
    rc.rc_mode = MPP_ENC_RC_MODE_CBR; rc.quality = MPP_ENC_RC_QUALITY_MEDIUM;
    rc.bps_target = codec_config.maxBitrate * 1000;
    rc.bps_max = rc.bps_target * 1.2; rc.bps_min = codec_config.minBitrate * 1000;
    rc.fps_in_num = codec_config.maxFramerate; rc.fps_in_denom = 1;
    rc.fps_out_num = codec_config.maxFramerate; rc.fps_out_denom = 1;
    rc.gop = codec_config.H264()->keyFrameInterval;
    mpp.control(ctx, MPP_ENC_SET_RC_CFG, &rc);

    ready = true;
    RTC_LOG(LS_INFO) << "MPP encoder ready " << w << "x" << h;
    return true;
  }

  int32_t EncodeOne(const VideoFrame& frame) {
    auto i420 = frame.video_frame_buffer()->ToI420();
    if (!i420) return WEBRTC_VIDEO_CODEC_ERROR;
    int ys = w * h, uvs = ys / 4;
    uint8_t* dst = (uint8_t*)mpp.mpp_buffer_get_ptr(frm_buf);
    memcpy(dst, i420->DataY(), ys);
    const uint8_t* u = i420->DataU(); const uint8_t* v = i420->DataV();
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunsafe-buffer-usage"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
    for (int i = 0; i < uvs; i++) { dst[ys + i*2] = u[i]; dst[ys + i*2 + 1] = v[i]; }
#pragma clang diagnostic pop
#pragma GCC diagnostic pop

    MppFrame f = nullptr; mpp.mpp_frame_init(&f);
    mpp.mpp_frame_set_width(f, w); mpp.mpp_frame_set_height(f, h);
    mpp.mpp_frame_set_hor_stride(f, w); mpp.mpp_frame_set_ver_stride(f, ALIGN(h, 16));
    mpp.mpp_frame_set_fmt(f, MPP_FMT_YUV420SP); mpp.mpp_frame_set_buffer(f, frm_buf);
    mpp.encode_put_frame(ctx, &mpi, f);

    MppPacket pkt = nullptr; mpp.encode_get_packet(ctx, &mpi, &pkt);
    if (!pkt) return WEBRTC_VIDEO_CODEC_OK;

    size_t len = mpp.mpp_packet_get_length(pkt); void* data = mpp.mpp_packet_get_data(pkt);
    if (!data || !len) { mpp.mpp_packet_deinit(pkt); return WEBRTC_VIDEO_CODEC_OK; }

    EncodedImage img;
    img.SetEncodedData(EncodedImageBuffer::Create((uint8_t*)data, len));
    img._frameType = VideoFrameType::kVideoFrameDelta;
    img.SetRtpTimestamp(frame.rtp_timestamp());
    img.capture_time_ms_ = frame.render_time_ms();
    if (callback) callback->OnEncodedImage(img, nullptr);
    mpp.mpp_packet_deinit(pkt);
    return WEBRTC_VIDEO_CODEC_OK;
  }

  void Release() {
    if (frm_buf) { mpp.mpp_buffer_put(frm_buf); frm_buf = nullptr; }
    if (buf_grp) { mpp.mpp_buffer_group_put(buf_grp); buf_grp = nullptr; }
    if (ctx) { mpp.mpp_destroy(ctx); ctx = nullptr; }
    ready = false;
  }
};

MppH264Encoder::~MppH264Encoder() = default;

int32_t MppH264Encoder::InitEncode(const VideoCodec* c, const Settings&) {
  impl_ = std::make_unique<Impl>(); impl_->codec_config = *c;
  if (!impl_->InitEncoder()) return WEBRTC_VIDEO_CODEC_ERROR;
  return WEBRTC_VIDEO_CODEC_OK;
}
int32_t MppH264Encoder::Encode(const VideoFrame& f, const std::vector<VideoFrameType>*) {
  return impl_ && impl_->ready ? impl_->EncodeOne(f) : WEBRTC_VIDEO_CODEC_UNINITIALIZED;
}
int32_t MppH264Encoder::RegisterEncodeCompleteCallback(EncodedImageCallback* cb) {
  if (impl_) impl_->callback = cb; return WEBRTC_VIDEO_CODEC_OK;
}
int32_t MppH264Encoder::Release() { impl_.reset(); return WEBRTC_VIDEO_CODEC_OK; }
void MppH264Encoder::SetRates(const RateControlParameters& p) {
  if (impl_ && impl_->ctx) { MppEncRcCfg_t rc = {}; rc.change = MPP_ENC_RC_CFG_CHANGE_BPS;
    rc.bps_target = p.bitrate.get_sum_kbps() * 1000; mpp.control(impl_->ctx, MPP_ENC_SET_RC_CFG, &rc); }
}
VideoEncoder::EncoderInfo MppH264Encoder::GetEncoderInfo() const {
  EncoderInfo i; i.implementation_name = "RK3588 MPP H264";
  i.is_hardware_accelerated = true; i.scaling_settings = VideoEncoder::ScalingSettings::kOff;
  return i;
}

}  // namespace webrtc
