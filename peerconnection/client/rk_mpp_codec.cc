// rk_mpp_codec.cc — RK3588 MPP H264 hardware encoder/decoder via dlopen

// MPP memory operations (I420↔NV12 conversion) require raw buffer access.
// Disable unsafe-buffer warnings for this file.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"

#include "apps/peerconnection/client/rk_mpp_codec.h"

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

// ── Real MPP headers from SDK ─────────────────────────
extern "C" {
#include "mpp_buffer.h"
#include "mpp_err.h"
#include "mpp_frame.h"
#include "mpp_meta.h"
#include "mpp_packet.h"
#include "rk_mpi.h"
#include "rk_mpi_cmd.h"
#include "rk_venc_cfg.h"
#include "rk_venc_cmd.h"
}


// ── Runtime MPP loading via dlsym (provided by rk_mpp_stubs.cc) ──
bool MppLibLoad();

static bool MppLoad() {
  return MppLibLoad();
}



#define ALIGN(x,a) (((x)+(a)-1)&~((a)-1))
#define MAX_RETRY 30

// MPP command values from rk_mpi_cmd.h — verified by official mpi_enc_test

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
  RTC_LOG(LS_INFO) << "MppH264EncoderTemplateAdapter::CreateEncoder";
  if (!MppLoad()) { RTC_LOG(LS_INFO) << "  CreateEncoder: MppLoad failed"; return nullptr; }
  RTC_LOG(LS_INFO) << "  CreateEncoder: OK";
  return std::make_unique<MppH264Encoder>();
}

std::unique_ptr<VideoDecoder> MppH264DecoderTemplateAdapter::CreateDecoder(
    const Environment&, const SdpVideoFormat&) {
  RTC_LOG(LS_INFO) << "MppH264DecoderTemplateAdapter::CreateDecoder";
  if (!MppLoad()) { RTC_LOG(LS_INFO) << "  CreateDecoder: MppLoad failed"; return nullptr; }
  RTC_LOG(LS_INFO) << "  CreateDecoder: OK";
  return std::make_unique<MppH264Decoder>();
}

// ── MppH264Encoder ────────────────────────────────────────────

struct MppH264Encoder::Impl {
  MppCtx ctx = nullptr;
  MppApi* mpi = nullptr;
  MppBufferGroup buf_grp = nullptr;
  MppBuffer frm_buf = nullptr;
  MppBuffer pkt_buf = nullptr;
  EncodedImageCallback* callback = nullptr;
  VideoCodec codec_config = {};
  size_t frame_size = 0;
  int w = 0, h = 0, hor_stride = 0, ver_stride = 0;
  bool ready = false;
  bool needs_reinit = false;  // defer reinit to Encode() for dynamic resolution
  std::vector<uint8_t> sps_pps_;

  ~Impl() { RTC_LOG(LS_INFO) << "~Impl() enc"; Release(); }

  void Release() {
    RTC_LOG(LS_INFO) << "  Release: start ready=" << ready;
    ready = false;
    frm_buf = nullptr;
    pkt_buf = nullptr;
    if (ctx && mpi) {
      RTC_LOG(LS_INFO) << "  Release: calling reset...";
      mpi->reset(ctx);
      RTC_LOG(LS_INFO) << "  Release: reset done";
    }
    if (buf_grp) {
      RTC_LOG(LS_INFO) << "  Release: buf_grp_put...";
      mpp_buffer_group_put(buf_grp);
      buf_grp = nullptr;
      RTC_LOG(LS_INFO) << "  Release: buf_grp_put done";
    }
    if (ctx) {
      RTC_LOG(LS_INFO) << "  Release: mpp_destroy...";
      mpp_destroy(ctx);
      ctx = nullptr;
      RTC_LOG(LS_INFO) << "  Release: mpp_destroy done";
    }
    RTC_LOG(LS_INFO) << "  Release: done";
  }

  bool InitEncoder() {
    Release();  // free previous resources on reconfig
    if (!MppLibLoad()) { RTC_LOG(LS_ERROR) << "MppLibLoad failed"; return false; }
    RTC_LOG(LS_INFO) << "MPP InitEncoder start";
    MPP_RET ret;

    w = codec_config.width;
    h = codec_config.height;
    hor_stride = ALIGN(w, 8);
    ver_stride = ALIGN(h, 16);
    frame_size = hor_stride * ver_stride * 3 / 2;
    RTC_LOG(LS_INFO) << "  frame " << w << "x" << h << " stride=" << hor_stride
                     << "x" << ver_stride << " size=" << frame_size;

    // Step 1: Create MPP context
    RTC_LOG(LS_INFO) << "  [1/5] mpp_create...";
    ret = mpp_create(&ctx, &mpi);
    RTC_LOG(LS_INFO) << "  mpp_create ret=" << ret << " ctx=" << (void*)ctx << " mpi=" << (void*)mpi;
    if (ret != MPP_OK) { RTC_LOG(LS_ERROR) << "mpp_create enc failed"; return false; }

    // Step 2: Init encoder
    RTC_LOG(LS_INFO) << "  [2/5] mpp_init ENC AVC...";
    ret = mpp_init(ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC);
    RTC_LOG(LS_INFO) << "  mpp_init ret=" << ret;
    if (ret != MPP_OK) { RTC_LOG(LS_ERROR) << "mpp_init enc failed"; return false; }

    // Step 3: Buffer group AFTER init (rk_h264_test order)
    RTC_LOG(LS_INFO) << "  [3/5] buffer group DRM...";
    ret = mpp_buffer_group_get(&buf_grp, MPP_BUFFER_TYPE_DRM,
                               MPP_BUFFER_INTERNAL, MODULE_TAG, __func__);
    RTC_LOG(LS_INFO) << "  buf_grp DRM ret=" << ret << " grp=" << (void*)buf_grp;
    if (ret) {
      RTC_LOG(LS_INFO) << "  buf_grp try ION...";
      ret = mpp_buffer_group_get(&buf_grp, MPP_BUFFER_TYPE_ION,
                                 MPP_BUFFER_INTERNAL, MODULE_TAG, __func__);
      RTC_LOG(LS_INFO) << "  buf_grp ION ret=" << ret << " grp=" << (void*)buf_grp;
    }
    if (!buf_grp) { RTC_LOG(LS_ERROR) << "buffer group failed"; return false; }
    mpp_buffer_group_limit_config(buf_grp, frame_size, 4);

    RTC_LOG(LS_INFO) << "  getting frame buffer size=" << frame_size;
    ret = mpp_buffer_get_with_tag(buf_grp, &frm_buf, frame_size, MODULE_TAG, __func__);
    RTC_LOG(LS_INFO) << "  frm_buf ret=" << ret << " buf=" << (void*)frm_buf;
    if (ret) { RTC_LOG(LS_ERROR) << "frm_buf failed"; return false; }

    // Output packet buffer (official mpi_enc_test pattern)
    ret = mpp_buffer_get_with_tag(buf_grp, &pkt_buf, frame_size, MODULE_TAG, __func__);
    RTC_LOG(LS_INFO) << "  pkt_buf ret=" << ret << " buf=" << (void*)pkt_buf;
    if (ret) { RTC_LOG(LS_ERROR) << "pkt_buf failed"; return false; }

    // Step 4: Try encoder with defaults only — ENC_SET_CFG returns -2
    // on the board's older library. Set minimal config and move on.
    RTC_LOG(LS_INFO) << "  [4/4] minimal config, skipping ENC_SET_CFG...";
    {
      MppEncCfg cfg = nullptr;
      mpp_enc_cfg_init(&cfg);
      mpp_enc_cfg_set_s32(cfg, "prep:width", w);
      mpp_enc_cfg_set_s32(cfg, "prep:height", h);
      mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", hor_stride);
      mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", ver_stride);
      mpp_enc_cfg_set_s32(cfg, "prep:format", MPP_FMT_YUV420SP);
      int bps = codec_config.maxBitrate * 1000;
      mpp_enc_cfg_set_s32(cfg, "rc:mode", 1);
      mpp_enc_cfg_set_s32(cfg, "rc:bps_target", bps);
      mpp_enc_cfg_set_s32(cfg, "rc:gop", 60);
      mpp_enc_cfg_set_s32(cfg, "codec:type", MPP_VIDEO_CodingAVC);
      MPP_RET cfg_ret = mpi->control(ctx, MPP_ENC_SET_CFG, cfg);
      RTC_LOG(LS_INFO) << "  ENC_SET_CFG ret=" << cfg_ret;
      mpp_enc_cfg_deinit(cfg);
    }

    ready = true;
    RTC_LOG(LS_INFO) << "MPP encoder ready " << w << "x" << h
                     << " SPS/PPS=" << sps_pps_.size() << "B";
    return true;
  }

  int32_t EncodeOne(const VideoFrame& frame,
                    const std::vector<VideoFrameType>* frame_types) {
    RTC_LOG(LS_INFO) << "  EncodeOne: start";
    // Only encode frames matching the configured resolution.
    // Quality scaler may downscale frames that MPP can't handle at
    // current bitrate; skip them rather than reconfiguring.
    int fw = frame.width(), fh = frame.height();
    if (fw != w || fh != h) {
      RTC_LOG(LS_INFO) << "  EncodeOne: skip frame " << fw << "x" << fh
                       << " (configured for " << w << "x" << h << ")";
      return WEBRTC_VIDEO_CODEC_OK;
    }
    scoped_refptr<I420BufferInterface> i420 =
        frame.video_frame_buffer()->ToI420();
    if (!i420) { RTC_LOG(LS_ERROR) << "  ToI420 failed"; return WEBRTC_VIDEO_CODEC_ERROR; }

    int ys = w * h, uvs = ys / 4;
    RTC_LOG(LS_INFO) << "  EncodeOne: get buf ptr...";
    uint8_t* dst = (uint8_t*)mpp_buffer_get_ptr_with_caller(frm_buf, __func__);
    RTC_LOG(LS_INFO) << "  EncodeOne: buf ptr=" << (void*)dst << " ys=" << ys;
    memcpy(dst, i420->DataY(), ys);
    const uint8_t* u = i420->DataU();
    const uint8_t* v = i420->DataV();
    for (int i = 0; i < uvs; i++) {
      dst[ys + i*2]     = u[i];
      dst[ys + i*2 + 1] = v[i];
    }
    RTC_LOG(LS_INFO) << "  EncodeOne: I420→NV12 done";

    // rk_h264_test doesn't call ENC_SET_IDR_FRAME; skip it

    RTC_LOG(LS_INFO) << "  EncodeOne: frame init...";
    MppFrame frm = nullptr;
    mpp_frame_init(&frm);
    mpp_frame_set_width(frm, w);
    mpp_frame_set_height(frm, h);
    mpp_frame_set_hor_stride(frm, hor_stride);
    mpp_frame_set_ver_stride(frm, ver_stride);
    mpp_frame_set_fmt(frm, MPP_FMT_YUV420SP);
    mpp_frame_set_buffer(frm, frm_buf);

    // Official pattern: pre-allocated packet buffer, attach to frame metadata
    MppPacket pkt = nullptr;
    mpp_packet_init_with_buffer(&pkt, pkt_buf);
    mpp_packet_set_length(pkt, 0);
    MppMeta meta = mpp_frame_get_meta(frm);
    mpp_meta_set_packet(meta, KEY_OUTPUT_PACKET, pkt);

    RTC_LOG(LS_INFO) << "  EncodeOne: put_frame...";
    mpi->encode_put_frame(ctx, frm);
    RTC_LOG(LS_INFO) << "  EncodeOne: put_frame done, deinit frame...";
    mpp_frame_deinit(&frm);
    RTC_LOG(LS_INFO) << "  EncodeOne: get packet loop (MAX_RETRY=" << MAX_RETRY << ")...";

    int tries = 0;
    for (; tries < MAX_RETRY; tries++) {
      MPP_RET ret = mpi->encode_get_packet(ctx, &pkt);
      if (tries == 0) {
        RTC_LOG(LS_INFO) << "  EncodeOne: 1st get_packet ret=" << ret
                         << " pkt=" << (void*)pkt
                         << " len=" << (pkt ? mpp_packet_get_length(pkt) : 0);
      }
      if (ret == MPP_OK && mpp_packet_get_length(pkt) > 0) break;
      usleep(1000);
    }
    RTC_LOG(LS_INFO) << "  EncodeOne: tries=" << tries
                     << " pkt=" << (void*)pkt
                     << " len=" << (pkt ? mpp_packet_get_length(pkt) : 0);
    if (tries >= MAX_RETRY || !pkt) {
      mpp_packet_deinit(&pkt);
      RTC_LOG(LS_INFO) << "  EncodeOne: no output (dropped)";
      return WEBRTC_VIDEO_CODEC_OK;
    }

    size_t len = mpp_packet_get_length(pkt);
    void*  data = mpp_packet_get_pos(pkt);
    if (!data || !len) {
      mpp_packet_deinit(&pkt);
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

    if (callback) {
      RTC_LOG(LS_INFO) << "  EncodeOne: calling callback...";
      callback->OnEncodedImage(img, nullptr);
      RTC_LOG(LS_INFO) << "  EncodeOne: callback done";
    }
    RTC_LOG(LS_INFO) << "  EncodeOne: deinit pkt...";
    mpp_packet_deinit(&pkt);
    RTC_LOG(LS_INFO) << "  EncodeOne: done, returning OK";
    return WEBRTC_VIDEO_CODEC_OK;
  }
};

MppH264Encoder::MppH264Encoder() : impl_(std::make_unique<Impl>()) {}
MppH264Encoder::~MppH264Encoder() = default;

int32_t MppH264Encoder::InitEncode(const VideoCodec* c, const Settings&) {
  impl_->codec_config = *c;
  if (impl_->ready) {
    // Quality scaler changed resolution without Release() first.
    // Defer reinit to next Encode() to avoid nested reinit in callback.
    RTC_LOG(LS_INFO) << "InitEncode: defer reinit " << c->width << "x" << c->height;
    impl_->needs_reinit = true;
    return WEBRTC_VIDEO_CODEC_OK;
  }
  // Release() was called first (ready=false). Do full reinit now.
  impl_->needs_reinit = false;
  if (!impl_->InitEncoder()) return WEBRTC_VIDEO_CODEC_ERROR;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t MppH264Encoder::Encode(const VideoFrame& f,
                               const std::vector<VideoFrameType>* frame_types) {
  RTC_LOG(LS_INFO) << "Encode called, ready=" << (impl_ && impl_->ready);
  if (impl_ && impl_->needs_reinit) {
    RTC_LOG(LS_INFO) << "Encode: applying deferred reinit";
    impl_->Release();
    if (!impl_->InitEncoder()) return WEBRTC_VIDEO_CODEC_ERROR;
    impl_->needs_reinit = false;
  }
  int32_t ret = impl_ && impl_->ready ? impl_->EncodeOne(f, frame_types)
                                      : WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  RTC_LOG(LS_INFO) << "Encode returned " << ret;
  return ret;
}

int32_t MppH264Encoder::RegisterEncodeCompleteCallback(
    EncodedImageCallback* cb) {
  if (impl_) impl_->callback = cb;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t MppH264Encoder::Release() {
  RTC_LOG(LS_INFO) << "MppH264Encoder::Release() — deferred";
  if (impl_) { impl_->ready = false; impl_->needs_reinit = true; }
  return WEBRTC_VIDEO_CODEC_OK;
}

void MppH264Encoder::SetRates(const RateControlParameters& p) {
  if (!impl_ || !impl_->ctx || !impl_->mpi) return;
  int bps = p.bitrate.get_sum_kbps() * 1000;
  MppEncCfg cfg = nullptr;
  mpp_enc_cfg_init(&cfg);
  mpp_enc_cfg_set_s32(cfg, "rc:bps_target", bps);
  mpp_enc_cfg_set_s32(cfg, "rc:bps_max", bps * 12 / 10);
  mpp_enc_cfg_set_s32(cfg, "rc:bps_min", bps / 2);
  impl_->mpi->control(impl_->ctx, MPP_ENC_SET_CFG, cfg);
  mpp_enc_cfg_deinit(cfg);
}

VideoEncoder::EncoderInfo MppH264Encoder::GetEncoderInfo() const {
  EncoderInfo i;
  i.implementation_name = "RK3588 MPP H264";
  i.is_hardware_accelerated = true;
  i.scaling_settings = VideoEncoder::ScalingSettings::kOff;
  // Prevent quality scaler from downscaling below configured resolution.
  // MPP encoder needs reinit on resolution change; keeping it stable
  // avoids the reconfiguration cascade.
  if (impl_) {
    i.scaling_settings.min_pixels_per_frame = impl_->w * impl_->h;
  }
  return i;
}

// ── MppH264Decoder ────────────────────────────────────────────

struct MppH264Decoder::Impl {
  MppCtx ctx = nullptr;
  MppApi* mpi = nullptr;
  MppBufferGroup frm_grp = nullptr;
  int w = 0, h = 0, hor_stride = 0, ver_stride = 0;
  size_t buf_size = 0;
  bool info_ready = false;
  DecodedImageCallback* callback = nullptr;

  ~Impl() { Release(); }

  void Release() {
    info_ready = false;
    if (ctx && mpi) { mpi->reset(ctx); }
    if (frm_grp) { mpp_buffer_group_put(frm_grp); frm_grp = nullptr; }
    if (ctx)      { mpp_destroy(ctx); ctx = nullptr; }
  }

  bool InitDecoder() {
    Release();  // free previous resources on reconfig
    if (!MppLibLoad()) { RTC_LOG(LS_ERROR) << "MppLibLoad failed"; return false; }
    MPP_RET ret;
    ret = mpp_create(&ctx, &mpi);
    if (ret) { RTC_LOG(LS_ERROR) << "mpp_create dec failed"; return false; }
    ret = mpp_init(ctx, MPP_CTX_DEC, MPP_VIDEO_CodingAVC);
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
    mpp_packet_init(&packet, (void*)input_image.data(), input_image.size());

    int pkt_done = 0;
    for (int tries = 0; tries < MAX_RETRY; tries++) {
      ret = mpi->decode_put_packet(ctx, packet);
      if (ret == MPP_OK) { pkt_done = 1; break; }
      usleep(1000);
    }
    mpp_packet_deinit(&packet);
    if (!pkt_done) return WEBRTC_VIDEO_CODEC_ERROR;

    MppFrame frame = nullptr;
    for (int tries = 0; tries < MAX_RETRY; tries++) {
      ret = mpi->decode_get_frame(ctx, &frame);
      if (ret == MPP_OK && frame) break;
      usleep(1000);
    }
    if (!frame) return WEBRTC_VIDEO_CODEC_OK;

    if (mpp_frame_get_info_change(frame)) {
      w = mpp_frame_get_width(frame);
      h = mpp_frame_get_height(frame);
      hor_stride = mpp_frame_get_hor_stride(frame);
      ver_stride = mpp_frame_get_ver_stride(frame);
      buf_size = mpp_frame_get_buf_size(frame);

      RTC_LOG(LS_INFO) << "Decoder info change: " << w << "x" << h
                       << " stride=" << hor_stride << "x" << ver_stride
                       << " buf=" << buf_size;

      if (frm_grp) mpp_buffer_group_put(frm_grp);
      ret = mpp_buffer_group_get(&frm_grp, MPP_BUFFER_TYPE_DRM, MPP_BUFFER_INTERNAL, "rk", __func__);
      if (ret)
        ret = mpp_buffer_group_get(&frm_grp, MPP_BUFFER_TYPE_ION, MPP_BUFFER_INTERNAL, "rk", __func__);
      mpp_buffer_group_limit_config(frm_grp, buf_size, 24);

      mpi->control(ctx, MPP_DEC_SET_EXT_BUF_GROUP, frm_grp);
      mpi->control(ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
      info_ready = true;

      mpp_frame_deinit(&frame);
      return WEBRTC_VIDEO_CODEC_OK;
    }

    if (!info_ready) {
      mpp_frame_deinit(&frame);
      return WEBRTC_VIDEO_CODEC_OK;
    }

    MppBuffer dec_buf = mpp_frame_get_buffer(frame);
    if (!dec_buf) {
      mpp_frame_deinit(&frame);
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    uint8_t* src = (uint8_t*)mpp_buffer_get_ptr_with_caller(dec_buf, __func__);
    scoped_refptr<I420Buffer> i420 = I420Buffer::Create(w, h);
    if (!i420) {
      mpp_frame_deinit(&frame);
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

    mpp_frame_deinit(&frame);
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

int32_t MppH264Decoder::Release() {
  if (impl_) impl_->Release();  // clean up MPP resources, keep impl_ alive
  return WEBRTC_VIDEO_CODEC_OK;
}

VideoDecoder::DecoderInfo MppH264Decoder::GetDecoderInfo() const {
  DecoderInfo i;
  i.implementation_name = "RK3588 MPP H264";
  i.is_hardware_accelerated = true;
  return i;
}

}  // namespace webrtc
