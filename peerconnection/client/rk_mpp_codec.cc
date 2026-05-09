// rk_mpp_codec.cc — RK3588 MPP H264 hardware encoder/decoder via dlopen

// MPP memory operations (I420↔NV12 conversion) require raw buffer access.
// Disable unsafe-buffer warnings for this file.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"

#include "apps/peerconnection/client/rk_mpp_codec.h"

#include <dlfcn.h>
#include <unistd.h>
#include <cstring>
#include <vector>

#include "api/scoped_refptr.h"
#include "api/video/encoded_image.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "modules/video_coding/codecs/h264/include/h264.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"

// ── MPP types (self-contained, no system header deps) ─────────
typedef int32_t MPP_RET;
#define MPP_OK 0
typedef void* MppCtx;
typedef void* MppBufferGroup;
typedef void* MppBuffer;
typedef void* MppFrame;
typedef void* MppPacket;
typedef void* MppTask;
typedef void* MppEncCfg;
typedef void* MppParam;
typedef uint32_t RK_U32;
typedef uint32_t RK_S32;
typedef int32_t  RK_S32_Signed;
typedef uint32_t MppCodingType;
typedef uint32_t MppFrameFormat;
typedef uint32_t MppCtxType;
typedef uint32_t MppPollType;
typedef uint32_t MppPortType;
typedef uint32_t MpiCmd;

enum {
  MPP_VIDEO_CodingAVC = 7,
  MPP_CTX_ENC         = 0,
  MPP_CTX_DEC         = 1,
  MPP_POLL_BLOCK      = 0,
  MPP_BUFFER_TYPE_DRM = 0,
  MPP_BUFFER_TYPE_ION = 1,
  MPP_FMT_YUV420SP    = 0x00000100,
};

// ── MppApi function pointer table (field order matches rk_mpi.h) ──
typedef struct MppApi_t {
  RK_U32  size;
  RK_U32  version;
  MPP_RET (*decode)(MppCtx, MppPacket, MppFrame*);
  MPP_RET (*decode_put_packet)(MppCtx, MppPacket);
  MPP_RET (*decode_get_frame)(MppCtx, MppFrame*);
  MPP_RET (*encode)(MppCtx, MppFrame, MppPacket*);
  MPP_RET (*encode_put_frame)(MppCtx, MppFrame);
  MPP_RET (*encode_get_packet)(MppCtx, MppPacket*);
  MPP_RET (*isp)(MppCtx, MppFrame, MppFrame);
  MPP_RET (*isp_put_frame)(MppCtx, MppFrame);
  MPP_RET (*isp_get_frame)(MppCtx, MppFrame*);
  MPP_RET (*poll)(MppCtx, MppPortType, MppPollType);
  MPP_RET (*dequeue)(MppCtx, MppPortType, MppTask*);
  MPP_RET (*enqueue)(MppCtx, MppPortType, MppTask);
  MPP_RET (*reset)(MppCtx);
  MPP_RET (*control)(MppCtx, MpiCmd, MppParam);
  RK_U32 reserv[16];
} MppApi_t;
typedef MppApi_t* MppApi;

// ── Function pointer types for standalone exports ─────────────
typedef MPP_RET (*MppCreateFn)(MppCtx*, MppApi*);
typedef MPP_RET (*MppInitFn)(MppCtx, MppCtxType, MppCodingType);
typedef MPP_RET (*MppDestroyFn)(MppCtx);
typedef MPP_RET (*MppBufGroupGetFn)(MppBufferGroup*, RK_U32, RK_S32, const char*, const char*);
typedef MPP_RET (*MppBufGroupLimitFn)(MppBufferGroup, size_t, int);
typedef MPP_RET (*MppBufGroupPutFn)(MppBufferGroup);
typedef MPP_RET (*MppBufGetFn)(MppBufferGroup, MppBuffer*, size_t, const char*, const char*);
typedef MPP_RET (*MppBufPutFn)(MppBuffer, const char*);
typedef void*    (*MppBufGetPtrFn)(MppBuffer, const char*);
typedef MPP_RET (*MppFrmInitFn)(MppFrame*);
typedef MPP_RET (*MppFrmDeinitFn)(MppFrame);
typedef MPP_RET (*MppFrmSetWidthFn)(MppFrame, RK_U32);
typedef MPP_RET (*MppFrmSetHeightFn)(MppFrame, RK_U32);
typedef MPP_RET (*MppFrmSetHorStrideFn)(MppFrame, RK_U32);
typedef MPP_RET (*MppFrmSetVerStrideFn)(MppFrame, RK_U32);
typedef MPP_RET (*MppFrmSetBufFn)(MppFrame, MppBuffer);
typedef MPP_RET (*MppFrmSetFmtFn)(MppFrame, MppFrameFormat);
typedef MPP_RET (*MppFrmSetEosFn)(MppFrame, RK_U32);
typedef MppBuffer (*MppFrmGetBufFn)(MppFrame);
typedef MPP_RET   (*MppFrmGetInfoChangeFn)(MppFrame);
typedef RK_U32    (*MppFrmGetWidthFn)(MppFrame);
typedef RK_U32    (*MppFrmGetHeightFn)(MppFrame);
typedef RK_U32    (*MppFrmGetHorStrideFn)(MppFrame);
typedef RK_U32    (*MppFrmGetVerStrideFn)(MppFrame);
typedef size_t    (*MppFrmGetBufSizeFn)(MppFrame);
typedef MPP_RET (*MppPktInitFn)(MppPacket*, void*, size_t);
typedef MPP_RET (*MppPktDeinitFn)(MppPacket);
typedef MPP_RET (*MppPktSetEosFn)(MppPacket);
typedef size_t  (*MppPktGetLenFn)(MppPacket);
typedef void*   (*MppPktGetPosFn)(MppPacket);
typedef MPP_RET (*MppPktSetLenFn)(MppPacket, size_t);
typedef MPP_RET (*MppEncCfgInitFn)(MppEncCfg*);
typedef MPP_RET (*MppEncCfgDeinitFn)(MppEncCfg);
typedef MPP_RET (*MppEncCfgSetS32Fn)(MppEncCfg, const char*, RK_S32_Signed);

// ── Global function pointer table ─────────────────────────────
static struct {
  void* lib = nullptr;
  MppCreateFn         mpp_create;
  MppInitFn           mpp_init;
  MppDestroyFn        mpp_destroy;
  MppBufGroupGetFn    mpp_buffer_group_get;
  MppBufGroupLimitFn  mpp_buffer_group_limit_config;
  MppBufGroupPutFn    mpp_buffer_group_put;
  MppBufGetFn         mpp_buffer_get_with_tag;
  MppBufPutFn         mpp_buffer_put_with_caller;
  MppBufGetPtrFn      mpp_buffer_get_ptr_with_caller;
  MppFrmInitFn        mpp_frame_init;
  MppFrmDeinitFn      mpp_frame_deinit;
  MppFrmSetWidthFn    mpp_frame_set_width;
  MppFrmSetHeightFn   mpp_frame_set_height;
  MppFrmSetHorStrideFn mpp_frame_set_hor_stride;
  MppFrmSetVerStrideFn mpp_frame_set_ver_stride;
  MppFrmSetBufFn      mpp_frame_set_buffer;
  MppFrmSetFmtFn      mpp_frame_set_fmt;
  MppFrmSetEosFn      mpp_frame_set_eos;
  MppFrmGetBufFn      mpp_frame_get_buffer;
  MppFrmGetInfoChangeFn mpp_frame_get_info_change;
  MppFrmGetWidthFn    mpp_frame_get_width;
  MppFrmGetHeightFn   mpp_frame_get_height;
  MppFrmGetHorStrideFn mpp_frame_get_hor_stride;
  MppFrmGetVerStrideFn mpp_frame_get_ver_stride;
  MppFrmGetBufSizeFn  mpp_frame_get_buf_size;
  MppPktInitFn        mpp_packet_init;
  MppPktDeinitFn      mpp_packet_deinit;
  MppPktSetEosFn      mpp_packet_set_eos;
  MppPktGetLenFn      mpp_packet_get_length;
  MppPktGetPosFn      mpp_packet_get_pos;
  MppPktSetLenFn      mpp_packet_set_length;
  MppEncCfgInitFn     mpp_enc_cfg_init;
  MppEncCfgDeinitFn   mpp_enc_cfg_deinit;
  MppEncCfgSetS32Fn   mpp_enc_cfg_set_s32;
} mpp;

#define LOAD(fn) do { \
    mpp.fn = (decltype(mpp.fn))dlsym(mpp.lib, #fn); \
    if (!mpp.fn) { \
      RTC_LOG(LS_ERROR) << "MPP dlsym failed: " #fn; \
      dlclose(mpp.lib); mpp.lib = nullptr; \
      return false; \
    } \
  } while(0)

static bool MppLoad() {
  if (mpp.lib) return true;
  mpp.lib = dlopen("librockchip_mpp.so.1", RTLD_NOW);
  if (!mpp.lib) {
    RTC_LOG(LS_WARNING) << "MPP not available, using software H264";
    return false;
  }
  LOAD(mpp_create); LOAD(mpp_init); LOAD(mpp_destroy);
  LOAD(mpp_buffer_group_get); LOAD(mpp_buffer_group_limit_config);
  LOAD(mpp_buffer_group_put);
  LOAD(mpp_buffer_get_with_tag); LOAD(mpp_buffer_put_with_caller);
  LOAD(mpp_buffer_get_ptr_with_caller);
  LOAD(mpp_frame_init); LOAD(mpp_frame_deinit);
  LOAD(mpp_frame_set_width); LOAD(mpp_frame_set_height);
  LOAD(mpp_frame_set_hor_stride); LOAD(mpp_frame_set_ver_stride);
  LOAD(mpp_frame_set_buffer); LOAD(mpp_frame_set_fmt);
  LOAD(mpp_frame_set_eos); LOAD(mpp_frame_get_buffer);
  LOAD(mpp_frame_get_info_change);
  LOAD(mpp_frame_get_width); LOAD(mpp_frame_get_height);
  LOAD(mpp_frame_get_hor_stride); LOAD(mpp_frame_get_ver_stride);
  LOAD(mpp_frame_get_buf_size);
  LOAD(mpp_packet_init); LOAD(mpp_packet_deinit); LOAD(mpp_packet_set_eos);
  LOAD(mpp_packet_get_length); LOAD(mpp_packet_get_pos);
  LOAD(mpp_packet_set_length);
  LOAD(mpp_enc_cfg_init); LOAD(mpp_enc_cfg_deinit);
  LOAD(mpp_enc_cfg_set_s32);
  RTC_LOG(LS_INFO) << "MPP loaded successfully";
  return true;
}
#undef LOAD

#define ALIGN(x,a) (((x)+(a)-1)&~((a)-1))
#define MAX_RETRY 30

// ── MpiCmd constants (from rk_mpi_cmd.h) ──────────────────────
enum {
  MPP_SET_OUTPUT_TIMEOUT         = 0,
  MPP_DEC_SET_PARSER_SPLIT_MODE  = 65,
  MPP_DEC_SET_EXT_BUF_GROUP      = 71,
  MPP_DEC_SET_INFO_CHANGE_READY  = 72,
  MPP_ENC_SET_CFG                = 121,
  MPP_ENC_GET_HDR_SYNC           = 130,
  MPP_ENC_SET_IDR_FRAME          = 138,
};

namespace webrtc {

// ── SDP format / adapter helpers ──────────────────────────────

std::vector<SdpVideoFormat> MppH264EncoderTemplateAdapter::SupportedFormats() {
  return {CreateH264Format(H264Profile::kProfileBaseline, H264Level::kLevel3_1, "1"),
          CreateH264Format(H264Profile::kProfileConstrainedBaseline, H264Level::kLevel3_1, "1")};
}

std::vector<SdpVideoFormat> MppH264DecoderTemplateAdapter::SupportedFormats() {
  return {CreateH264Format(H264Profile::kProfileBaseline, H264Level::kLevel3_1, "1"),
          CreateH264Format(H264Profile::kProfileConstrainedBaseline, H264Level::kLevel3_1, "1")};
}

std::unique_ptr<VideoEncoder> MppH264EncoderTemplateAdapter::CreateEncoder(
    const Environment&, const SdpVideoFormat&) {
  if (!MppLoad()) return nullptr;
  return std::make_unique<MppH264Encoder>();
}

std::unique_ptr<VideoDecoder> MppH264DecoderTemplateAdapter::CreateDecoder(
    const Environment&, const SdpVideoFormat&) {
  if (!MppLoad()) return nullptr;
  return std::make_unique<MppH264Decoder>();
}

// ── MppH264Encoder ────────────────────────────────────────────

struct MppH264Encoder::Impl {
  MppCtx ctx = nullptr;
  MppApi mpi = nullptr;
  MppBufferGroup buf_grp = nullptr;
  MppBuffer frm_buf = nullptr;
  EncodedImageCallback* callback = nullptr;
  VideoCodec codec_config = {};
  size_t frame_size = 0;
  int w = 0, h = 0, hor_stride = 0, ver_stride = 0;
  bool ready = false;
  std::vector<uint8_t> sps_pps_;

  ~Impl() { Release(); }

  void Release() {
    ready = false;
    if (frm_buf) { mpp.mpp_buffer_put_with_caller(frm_buf, __func__); frm_buf = nullptr; }
    if (buf_grp) { mpp.mpp_buffer_group_put(buf_grp); buf_grp = nullptr; }
    if (ctx)     { mpp.mpp_destroy(ctx); ctx = nullptr; }
  }

  bool InitEncoder() {
    MPP_RET ret;
    ret = mpp.mpp_create(&ctx, &mpi);
    if (ret != MPP_OK) { RTC_LOG(LS_ERROR) << "mpp_create enc failed"; return false; }
    ret = mpp.mpp_init(ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC);
    if (ret != MPP_OK) { RTC_LOG(LS_ERROR) << "mpp_init enc failed"; return false; }

    w = codec_config.width;
    h = codec_config.height;
    hor_stride = ALIGN(w, 8);
    ver_stride = ALIGN(h, 16);
    frame_size = hor_stride * ver_stride * 3 / 2;

    ret = mpp.mpp_buffer_group_get(&buf_grp, MPP_BUFFER_TYPE_DRM, 0, "rk", __func__);
    if (ret) ret = mpp.mpp_buffer_group_get(&buf_grp, MPP_BUFFER_TYPE_ION, 0, "rk", __func__);
    if (!buf_grp) { RTC_LOG(LS_ERROR) << "buffer group failed"; return false; }
    mpp.mpp_buffer_group_limit_config(buf_grp, frame_size, 4);

    ret = mpp.mpp_buffer_get_with_tag(buf_grp, &frm_buf, frame_size, "rk", __func__);
    if (ret) { RTC_LOG(LS_ERROR) << "frm_buf failed"; return false; }

    MppEncCfg cfg = nullptr;
    mpp.mpp_enc_cfg_init(&cfg);

    mpp.mpp_enc_cfg_set_s32(cfg, "prep:width", w);
    mpp.mpp_enc_cfg_set_s32(cfg, "prep:height", h);
    mpp.mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", hor_stride);
    mpp.mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", ver_stride);
    mpp.mpp_enc_cfg_set_s32(cfg, "prep:format", MPP_FMT_YUV420SP);

    int bps = codec_config.maxBitrate * 1000;
    mpp.mpp_enc_cfg_set_s32(cfg, "rc:mode", 1);  // CBR
    mpp.mpp_enc_cfg_set_s32(cfg, "rc:bps_target", bps);
    mpp.mpp_enc_cfg_set_s32(cfg, "rc:bps_max", bps * 12 / 10);
    mpp.mpp_enc_cfg_set_s32(cfg, "rc:bps_min", bps / 2);
    mpp.mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", codec_config.maxFramerate);
    mpp.mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denom", 1);
    mpp.mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num", codec_config.maxFramerate);
    mpp.mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denom", 1);
    mpp.mpp_enc_cfg_set_s32(cfg, "rc:gop", codec_config.H264()->keyFrameInterval);

    mpp.mpp_enc_cfg_set_s32(cfg, "codec:type", MPP_VIDEO_CodingAVC);
    mpp.mpp_enc_cfg_set_s32(cfg, "h264:profile", 100);
    mpp.mpp_enc_cfg_set_s32(cfg, "h264:level", 42);
    mpp.mpp_enc_cfg_set_s32(cfg, "h264:cabac_en", 1);
    mpp.mpp_enc_cfg_set_s32(cfg, "h264:trans8x8", 1);

    ret = mpi->control(ctx, MPP_ENC_SET_CFG, cfg);
    mpp.mpp_enc_cfg_deinit(cfg);
    if (ret) { RTC_LOG(LS_ERROR) << "ENC_SET_CFG failed"; return false; }

    // Get SPS/PPS header
    MppPacket hdr_pkt = nullptr;
    mpp.mpp_packet_init(&hdr_pkt, nullptr, 0);
    ret = mpi->control(ctx, MPP_ENC_GET_HDR_SYNC, hdr_pkt);
    if (ret == MPP_OK) {
      void* ptr = mpp.mpp_packet_get_pos(hdr_pkt);
      size_t len = mpp.mpp_packet_get_length(hdr_pkt);
      if (ptr && len > 0) {
        sps_pps_.assign((uint8_t*)ptr, (uint8_t*)ptr + len);
      }
    }
    mpp.mpp_packet_deinit(hdr_pkt);

    ready = true;
    RTC_LOG(LS_INFO) << "MPP encoder ready " << w << "x" << h
                     << " SPS/PPS=" << sps_pps_.size() << "B";
    return true;
  }

  int32_t EncodeOne(const VideoFrame& frame,
                    const std::vector<VideoFrameType>* frame_types) {
    scoped_refptr<I420BufferInterface> i420 =
        frame.video_frame_buffer()->ToI420();
    if (!i420) return WEBRTC_VIDEO_CODEC_ERROR;

    int ys = w * h, uvs = ys / 4;
    uint8_t* dst = (uint8_t*)mpp.mpp_buffer_get_ptr_with_caller(frm_buf, __func__);
    memcpy(dst, i420->DataY(), ys);
    const uint8_t* u = i420->DataU();
    const uint8_t* v = i420->DataV();
    for (int i = 0; i < uvs; i++) {
      dst[ys + i*2]     = u[i];
      dst[ys + i*2 + 1] = v[i];
    }

    // Request IDR if signaled
    if (frame_types) {
      for (auto ft : *frame_types) {
        if (ft == VideoFrameType::kVideoFrameKey) {
          mpi->control(ctx, MPP_ENC_SET_IDR_FRAME, nullptr);
          break;
        }
      }
    }

    MppFrame frm = nullptr;
    mpp.mpp_frame_init(&frm);
    mpp.mpp_frame_set_width(frm, w);
    mpp.mpp_frame_set_height(frm, h);
    mpp.mpp_frame_set_hor_stride(frm, hor_stride);
    mpp.mpp_frame_set_ver_stride(frm, ver_stride);
    mpp.mpp_frame_set_fmt(frm, MPP_FMT_YUV420SP);
    mpp.mpp_frame_set_buffer(frm, frm_buf);

    mpi->encode_put_frame(ctx, frm);
    mpp.mpp_frame_deinit(frm);

    MppPacket pkt = nullptr;
    mpp.mpp_packet_init(&pkt, nullptr, 0);
    mpp.mpp_packet_set_length(pkt, 0);

    int tries = 0;
    for (; tries < MAX_RETRY; tries++) {
      MPP_RET ret = mpi->encode_get_packet(ctx, &pkt);
      if (ret == MPP_OK && mpp.mpp_packet_get_length(pkt) > 0) break;
      usleep(1000);
    }
    if (tries >= MAX_RETRY || !pkt) {
      mpp.mpp_packet_deinit(pkt);
      return WEBRTC_VIDEO_CODEC_OK;
    }

    size_t len = mpp.mpp_packet_get_length(pkt);
    void*  data = mpp.mpp_packet_get_pos(pkt);
    if (!data || !len) {
      mpp.mpp_packet_deinit(pkt);
      return WEBRTC_VIDEO_CODEC_OK;
    }

    EncodedImage img;
    img.SetEncodedData(EncodedImageBuffer::Create((uint8_t*)data, len));
    img.SetRtpTimestamp(frame.rtp_timestamp());
    img.capture_time_ms_ = frame.render_time_ms();

    bool is_key = frame_types &&
        std::find(frame_types->begin(), frame_types->end(),
                  VideoFrameType::kVideoFrameKey) != frame_types->end();
    img._frameType = is_key ? VideoFrameType::kVideoFrameKey
                            : VideoFrameType::kVideoFrameDelta;

    if (callback) callback->OnEncodedImage(img, nullptr);
    mpp.mpp_packet_deinit(pkt);
    return WEBRTC_VIDEO_CODEC_OK;
  }
};

MppH264Encoder::MppH264Encoder() : impl_(std::make_unique<Impl>()) {}
MppH264Encoder::~MppH264Encoder() = default;

int32_t MppH264Encoder::InitEncode(const VideoCodec* c, const Settings&) {
  impl_->codec_config = *c;
  if (!impl_->InitEncoder()) return WEBRTC_VIDEO_CODEC_ERROR;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t MppH264Encoder::Encode(const VideoFrame& f,
                               const std::vector<VideoFrameType>* frame_types) {
  return impl_ && impl_->ready ? impl_->EncodeOne(f, frame_types)
                               : WEBRTC_VIDEO_CODEC_UNINITIALIZED;
}

int32_t MppH264Encoder::RegisterEncodeCompleteCallback(
    EncodedImageCallback* cb) {
  if (impl_) impl_->callback = cb;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t MppH264Encoder::Release() { impl_.reset(); return WEBRTC_VIDEO_CODEC_OK; }

void MppH264Encoder::SetRates(const RateControlParameters& p) {
  if (!impl_ || !impl_->ctx || !impl_->mpi) return;
  int bps = p.bitrate.get_sum_kbps() * 1000;
  MppEncCfg cfg = nullptr;
  mpp.mpp_enc_cfg_init(&cfg);
  mpp.mpp_enc_cfg_set_s32(cfg, "rc:bps_target", bps);
  mpp.mpp_enc_cfg_set_s32(cfg, "rc:bps_max", bps * 12 / 10);
  mpp.mpp_enc_cfg_set_s32(cfg, "rc:bps_min", bps / 2);
  impl_->mpi->control(impl_->ctx, MPP_ENC_SET_CFG, cfg);
  mpp.mpp_enc_cfg_deinit(cfg);
}

VideoEncoder::EncoderInfo MppH264Encoder::GetEncoderInfo() const {
  EncoderInfo i;
  i.implementation_name = "RK3588 MPP H264";
  i.is_hardware_accelerated = true;
  i.scaling_settings = VideoEncoder::ScalingSettings::kOff;
  return i;
}

// ── MppH264Decoder ────────────────────────────────────────────

struct MppH264Decoder::Impl {
  MppCtx ctx = nullptr;
  MppApi mpi = nullptr;
  MppBufferGroup frm_grp = nullptr;
  int w = 0, h = 0, hor_stride = 0, ver_stride = 0;
  size_t buf_size = 0;
  bool info_ready = false;
  DecodedImageCallback* callback = nullptr;

  ~Impl() { Release(); }

  void Release() {
    info_ready = false;
    if (frm_grp) { mpp.mpp_buffer_group_put(frm_grp); frm_grp = nullptr; }
    if (ctx)      { mpp.mpp_destroy(ctx); ctx = nullptr; }
  }

  bool InitDecoder() {
    MPP_RET ret;
    ret = mpp.mpp_create(&ctx, &mpi);
    if (ret) { RTC_LOG(LS_ERROR) << "mpp_create dec failed"; return false; }
    ret = mpp.mpp_init(ctx, MPP_CTX_DEC, MPP_VIDEO_CodingAVC);
    if (ret) { RTC_LOG(LS_ERROR) << "mpp_init dec failed"; return false; }

    RK_U32 split = 1;
    mpi->control(ctx, MPP_DEC_SET_PARSER_SPLIT_MODE, &split);

    info_ready = false;
    RTC_LOG(LS_INFO) << "MPP decoder ready";
    return true;
  }

  int32_t DecodeOne(const EncodedImage& input_image, int64_t render_time_ms) {
    MPP_RET ret;
    MppPacket packet = nullptr;
    mpp.mpp_packet_init(&packet, (void*)input_image.data(), input_image.size());

    int pkt_done = 0;
    for (int tries = 0; tries < MAX_RETRY; tries++) {
      ret = mpi->decode_put_packet(ctx, packet);
      if (ret == MPP_OK) { pkt_done = 1; break; }
      usleep(1000);
    }
    mpp.mpp_packet_deinit(packet);
    if (!pkt_done) return WEBRTC_VIDEO_CODEC_ERROR;

    MppFrame frame = nullptr;
    for (int tries = 0; tries < MAX_RETRY; tries++) {
      ret = mpi->decode_get_frame(ctx, &frame);
      if (ret == MPP_OK && frame) break;
      usleep(1000);
    }
    if (!frame) return WEBRTC_VIDEO_CODEC_OK;

    if (mpp.mpp_frame_get_info_change(frame)) {
      w = mpp.mpp_frame_get_width(frame);
      h = mpp.mpp_frame_get_height(frame);
      hor_stride = mpp.mpp_frame_get_hor_stride(frame);
      ver_stride = mpp.mpp_frame_get_ver_stride(frame);
      buf_size = mpp.mpp_frame_get_buf_size(frame);

      RTC_LOG(LS_INFO) << "Decoder info change: " << w << "x" << h
                       << " stride=" << hor_stride << "x" << ver_stride
                       << " buf=" << buf_size;

      if (frm_grp) mpp.mpp_buffer_group_put(frm_grp);
      ret = mpp.mpp_buffer_group_get(&frm_grp, MPP_BUFFER_TYPE_DRM, 0, "rk", __func__);
      if (ret)
        ret = mpp.mpp_buffer_group_get(&frm_grp, MPP_BUFFER_TYPE_ION, 0, "rk", __func__);
      mpp.mpp_buffer_group_limit_config(frm_grp, buf_size, 24);

      mpi->control(ctx, MPP_DEC_SET_EXT_BUF_GROUP, frm_grp);
      mpi->control(ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
      info_ready = true;

      mpp.mpp_frame_deinit(frame);
      return WEBRTC_VIDEO_CODEC_OK;
    }

    if (!info_ready) {
      mpp.mpp_frame_deinit(frame);
      return WEBRTC_VIDEO_CODEC_OK;
    }

    MppBuffer dec_buf = mpp.mpp_frame_get_buffer(frame);
    if (!dec_buf) {
      mpp.mpp_frame_deinit(frame);
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    uint8_t* src = (uint8_t*)mpp.mpp_buffer_get_ptr_with_caller(dec_buf, __func__);
    scoped_refptr<I420Buffer> i420 = I420Buffer::Create(w, h);
    if (!i420) {
      mpp.mpp_frame_deinit(frame);
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    int ys = w * h, uvs = ys / 4;
    memcpy(i420->MutableDataY(), src, ys);
    for (int i = 0; i < uvs; i++) {
      i420->MutableDataU()[i] = src[ys + i*2];
      i420->MutableDataV()[i] = src[ys + i*2 + 1];
    }

    VideoFrame decoded_frame = VideoFrame::Builder()
        .set_video_frame_buffer(i420)
        .set_rtp_timestamp(input_image.RtpTimestamp())
        .set_ntp_time_ms(input_image.NtpTimeMs())
        .build();

    if (callback) {
      callback->Decoded(decoded_frame, render_time_ms);
    }

    mpp.mpp_frame_deinit(frame);
    return WEBRTC_VIDEO_CODEC_OK;
  }
};

MppH264Decoder::MppH264Decoder() : impl_(std::make_unique<Impl>()) {}
MppH264Decoder::~MppH264Decoder() = default;

bool MppH264Decoder::Configure(const Settings&) {
  if (!impl_->InitDecoder()) return false;
  return true;
}

int32_t MppH264Decoder::Decode(const EncodedImage& input_image,
                               int64_t render_time_ms) {
  if (!impl_) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  return impl_->DecodeOne(input_image, render_time_ms);
}

int32_t MppH264Decoder::RegisterDecodeCompleteCallback(
    DecodedImageCallback* cb) {
  if (impl_) impl_->callback = cb;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t MppH264Decoder::Release() { impl_.reset(); return WEBRTC_VIDEO_CODEC_OK; }

VideoDecoder::DecoderInfo MppH264Decoder::GetDecoderInfo() const {
  DecoderInfo i;
  i.implementation_name = "RK3588 MPP H264";
  i.is_hardware_accelerated = true;
  return i;
}

}  // namespace webrtc
