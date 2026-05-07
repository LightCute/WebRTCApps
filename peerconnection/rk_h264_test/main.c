/*
 * rk_h264_test — MPP E2E pipeline test
 *   V4L2 camera → MPP H264 encode → MPP H264 decode → file output
 *   Measures capture/encode/decode latency and throughput.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <math.h>
#include <inttypes.h>

#include "rk_mpi.h"
#include "rk_venc_cfg.h"
#include "mpp_frame.h"
#include "mpp_packet.h"
#include "mpp_buffer.h"
#include "mpp_err.h"
#include "mpp_common.h"
#include "mpp_log.h"
#include "camera_source.h"

#define STATS_INTERVAL  30
#define MAX_RETRY       30
#define RETRY_MS        1

/* ── Stats ────────────────────────────────────────────────── */
typedef struct {
    int64_t ts_capture;
    int64_t ts_encoded;
    int64_t ts_decoded;
    int      enc_bytes;
} FrameStat;

typedef struct {
    FrameStat *entries;
    int        count;
    int        cap;
    int64_t    start_ns;
} Stats;

static Stats g_stats;

static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static double ns_to_ms(int64_t ns) { return (double)ns / 1000000.0; }

static void stats_init(int max_frames) {
    g_stats.entries = calloc((size_t)max_frames, sizeof(FrameStat));
    g_stats.cap = max_frames;
    g_stats.count = 0;
    g_stats.start_ns = now_ns();
}

static void stats_record(int64_t ts_cap, int64_t ts_enc, int64_t ts_dec, int bytes) {
    if (g_stats.count >= g_stats.cap) return;
    FrameStat *s = &g_stats.entries[g_stats.count++];
    s->ts_capture = ts_cap;
    s->ts_encoded = ts_enc;
    s->ts_decoded = ts_dec;
    s->enc_bytes  = bytes;
}

static void stats_report_periodic(int start, int end) {
    int n = end - start;
    if (n < 1) return;

    double dur = ns_to_ms(g_stats.entries[end-1].ts_capture -
                          g_stats.entries[start].ts_capture);
    double fps = (double)n / dur * 1000.0;

    double sum_enc = 0, sum_dec = 0, sum_e2e = 0;
    int64_t sum_bytes = 0;
    for (int i = start; i < end; i++) {
        sum_enc  += ns_to_ms(g_stats.entries[i].ts_encoded -
                             g_stats.entries[i].ts_capture);
        sum_dec  += ns_to_ms(g_stats.entries[i].ts_decoded -
                             g_stats.entries[i].ts_encoded);
        sum_e2e  += ns_to_ms(g_stats.entries[i].ts_decoded -
                             g_stats.entries[i].ts_capture);
        sum_bytes += g_stats.entries[i].enc_bytes;
    }
    double kbps = (double)sum_bytes * 8.0 / (dur / 1000.0) / 1000.0;

    fprintf(stderr,
        "[%4d-%4d] cap_fps=%5.1f  enc=%5.1fms  dec=%5.1fms  "
        "e2e=%5.1fms  bitrate=%6.0fkbps\n",
        start, end - 1, fps, sum_enc / n, sum_dec / n,
        sum_e2e / n, kbps);
}

static void stats_report_final(void) {
    int n = g_stats.count;
    if (n < 1) return;

    double dur_sec = ns_to_ms(g_stats.entries[n-1].ts_decoded -
                              g_stats.entries[0].ts_capture) / 1000.0;
    double fps = (double)n / dur_sec;

    double enc_min = 1e9, enc_max = 0, enc_sum = 0;
    double dec_min = 1e9, dec_max = 0, dec_sum = 0;
    double e2e_min = 1e9, e2e_max = 0, e2e_sum = 0;
    int64_t total_bytes = 0;

    for (int i = 0; i < n; i++) {
        double enc = ns_to_ms(g_stats.entries[i].ts_encoded -
                              g_stats.entries[i].ts_capture);
        double dec = ns_to_ms(g_stats.entries[i].ts_decoded -
                              g_stats.entries[i].ts_encoded);
        double e2e = ns_to_ms(g_stats.entries[i].ts_decoded -
                              g_stats.entries[i].ts_capture);

        if (enc < enc_min) enc_min = enc;
        if (enc > enc_max) enc_max = enc;
        if (dec < dec_min) dec_min = dec;
        if (dec > dec_max) dec_max = dec;
        if (e2e < e2e_min) e2e_min = e2e;
        if (e2e > e2e_max) e2e_max = e2e;

        enc_sum += enc; dec_sum += dec; e2e_sum += e2e;
        total_bytes += g_stats.entries[i].enc_bytes;
    }

    double enc_avg = enc_sum / n;
    double dec_avg = dec_sum / n;
    double e2e_avg = e2e_sum / n;
    double kbps = (double)total_bytes * 8.0 / dur_sec / 1000.0;

    fprintf(stderr, "\n══════ Final Stats ══════\n");
    fprintf(stderr, "Total frames:        %d\n", n);
    fprintf(stderr, "Duration:            %.2f s\n", dur_sec);
    fprintf(stderr, "Overall FPS:         %.1f\n", fps);
    fprintf(stderr, "Average bitrate:     %.0f kbps\n", kbps);
    fprintf(stderr, "Total encoded bytes: %" PRId64 "\n", total_bytes);
    fprintf(stderr, "\n");
    fprintf(stderr, "              avg      min      max\n");
    fprintf(stderr, "Encode(ms)  %7.2f  %7.2f  %7.2f\n",
            enc_avg, enc_min, enc_max);
    fprintf(stderr, "Decode(ms)  %7.2f  %7.2f  %7.2f\n",
            dec_avg, dec_min, dec_max);
    fprintf(stderr, "E2E(ms)     %7.2f  %7.2f  %7.2f\n",
            e2e_avg, e2e_min, e2e_max);
    fprintf(stderr, "══════════════════════════\n");
}

/* ── Encoder ───────────────────────────────────────────────── */
typedef struct {
    MppCtx          ctx;
    MppApi         *mpi;
    MppBufferGroup  buf_grp;
    MppBuffer       frm_buf;
    MppBuffer       pkt_buf;
    int             width, height;
    int             hor_stride, ver_stride;
    size_t          frame_size;
    size_t          packet_size;
} Enc;

static int enc_init(Enc *e, int width, int height, int fps, int bps_target) {
    MPP_RET ret;
    MppEncCfg cfg = NULL;

    ret = mpp_create(&e->ctx, &e->mpi);
    if (ret) { fprintf(stderr, "mpp_create enc failed %d\n", ret); return -1; }

    ret = mpp_init(e->ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC);
    if (ret) { fprintf(stderr, "mpp_init enc failed %d\n", ret); return -1; }

    e->width  = width;
    e->height = height;
    e->hor_stride = MPP_ALIGN(width, 8);
    e->ver_stride = MPP_ALIGN(height, 16);
    e->frame_size  = e->hor_stride * e->ver_stride * 3 / 2;
    e->packet_size = e->frame_size;

    ret = mpp_buffer_group_get_internal(&e->buf_grp, MPP_BUFFER_TYPE_DRM);
    if (ret)
        ret = mpp_buffer_group_get_internal(&e->buf_grp, MPP_BUFFER_TYPE_ION);
    if (ret || !e->buf_grp) {
        fprintf(stderr, "buffer group failed %d\n", ret); return -1;
    }
    mpp_buffer_group_limit_config(e->buf_grp, e->frame_size, 4);

    ret = mpp_buffer_get(e->buf_grp, &e->frm_buf, e->frame_size);
    if (ret) { fprintf(stderr, "frm_buf failed %d\n", ret); return -1; }

    ret = mpp_buffer_get(e->buf_grp, &e->pkt_buf, e->packet_size);
    if (ret) { fprintf(stderr, "pkt_buf failed %d\n", ret); return -1; }

    /* Use MppEncCfg key-value API (recommended) */
    ret = mpp_enc_cfg_init(&cfg);
    if (ret) { fprintf(stderr, "mpp_enc_cfg_init failed %d\n", ret); return -1; }

    mpp_enc_cfg_set_s32(cfg, "prep:width", width);
    mpp_enc_cfg_set_s32(cfg, "prep:height", height);
    mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", e->hor_stride);
    mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", e->ver_stride);
    mpp_enc_cfg_set_s32(cfg, "prep:format", MPP_FMT_YUV420SP);

    mpp_enc_cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_target", bps_target);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_max", bps_target * 12 / 10);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_min", bps_target / 2);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", fps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denom", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num", fps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denom", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:gop", fps);

    mpp_enc_cfg_set_s32(cfg, "codec:type", MPP_VIDEO_CodingAVC);
    mpp_enc_cfg_set_s32(cfg, "h264:profile", 100);
    mpp_enc_cfg_set_s32(cfg, "h264:level", 42);
    mpp_enc_cfg_set_s32(cfg, "h264:cabac_en", 1);
    mpp_enc_cfg_set_s32(cfg, "h264:trans8x8", 1);

    ret = e->mpi->control(e->ctx, MPP_ENC_SET_CFG, cfg);
    mpp_enc_cfg_deinit(cfg);
    if (ret) { fprintf(stderr, "MPP_ENC_SET_CFG failed %d\n", ret); return -1; }

    fprintf(stderr, "Encoder init OK: %dx%d %dbps GOP=%d\n",
            width, height, bps_target, fps);
    return 0;
}

static int enc_get_hdr(Enc *e, uint8_t **data, size_t *len) {
    MppPacket pkt = NULL;
    mpp_packet_init_with_buffer(&pkt, e->pkt_buf);
    mpp_packet_set_length(pkt, 0);
    MPP_RET ret = e->mpi->control(e->ctx, MPP_ENC_GET_HDR_SYNC, pkt);
    if (ret) { mpp_packet_deinit(&pkt); return -1; }
    *data = (uint8_t *)mpp_packet_get_pos(pkt);
    *len  = mpp_packet_get_length(pkt);
    if (*len == 0) { mpp_packet_deinit(&pkt); return -1; }
    uint8_t *copy = malloc(*len);
    memcpy(copy, *data, *len);
    *data = copy;
    mpp_packet_deinit(&pkt);
    return 0;
}

static int enc_encode(Enc *e, MppBuffer cam_buf,
                      uint8_t **out_data, size_t *out_len) {
    MppFrame frame = NULL;
    mpp_frame_init(&frame);
    mpp_frame_set_width(frame, e->width);
    mpp_frame_set_height(frame, e->height);
    mpp_frame_set_hor_stride(frame, e->hor_stride);
    mpp_frame_set_ver_stride(frame, e->ver_stride);
    mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
    mpp_frame_set_buffer(frame, cam_buf);

    MPP_RET ret = e->mpi->encode_put_frame(e->ctx, frame);
    mpp_frame_deinit(&frame);
    if (ret) { return -1; }

    MppPacket pkt = NULL;
    mpp_packet_init_with_buffer(&pkt, e->pkt_buf);
    mpp_packet_set_length(pkt, 0);

    for (int tries = 0; tries < MAX_RETRY; tries++) {
        ret = e->mpi->encode_get_packet(e->ctx, &pkt);
        if (ret == 0 && mpp_packet_get_length(pkt) > 0) break;
        usleep(RETRY_MS * 1000);
    }
    if (!pkt || mpp_packet_get_length(pkt) == 0) {
        mpp_packet_deinit(&pkt); return -1;
    }

    size_t len = mpp_packet_get_length(pkt);
    uint8_t *copy = malloc(len);
    memcpy(copy, mpp_packet_get_pos(pkt), len);
    *out_data = copy;
    *out_len  = len;
    mpp_packet_deinit(&pkt);
    return 0;
}

static void enc_deinit(Enc *e) {
    if (e->frm_buf) { mpp_buffer_put(e->frm_buf); e->frm_buf = NULL; }
    if (e->pkt_buf) { mpp_buffer_put(e->pkt_buf); e->pkt_buf = NULL; }
    if (e->buf_grp) { mpp_buffer_group_put(e->buf_grp); e->buf_grp = NULL; }
    if (e->ctx)     { mpp_destroy(e->ctx); e->ctx = NULL; }
}

/* ── Decoder ───────────────────────────────────────────────── */
typedef struct {
    MppCtx          ctx;
    MppApi         *mpi;
    MppBufferGroup  frm_grp;
    int             width, height;
    int             hor_stride, ver_stride;
    size_t          buf_size;
    int             info_ready;
    int             frame_count;
} Dec;

static int dec_init(Dec *d) {
    MPP_RET ret;
    ret = mpp_create(&d->ctx, &d->mpi);
    if (ret) { fprintf(stderr, "mpp_create dec failed %d\n", ret); return -1; }

    ret = mpp_init(d->ctx, MPP_CTX_DEC, MPP_VIDEO_CodingAVC);
    if (ret) { fprintf(stderr, "mpp_init dec failed %d\n", ret); return -1; }

    RK_U32 split = 1;
    d->mpi->control(d->ctx, MPP_DEC_SET_PARSER_SPLIT_MODE, &split);

    d->info_ready = 0;
    d->frame_count = 0;
    d->width = d->height = 0;
    fprintf(stderr, "Decoder init OK\n");
    return 0;
}

static int dec_decode(Dec *d, uint8_t *h264_data, size_t h264_len,
                      uint8_t **out_data, size_t *out_len, int is_eos) {
    MPP_RET ret;
    MppPacket packet = NULL;

    mpp_packet_init(&packet, h264_data, h264_len);
    if (is_eos)
        mpp_packet_set_eos(packet);

    int pkt_done = 0;
    for (int tries = 0; tries < MAX_RETRY; tries++) {
        ret = d->mpi->decode_put_packet(d->ctx, packet);
        if (ret == MPP_OK) { pkt_done = 1; break; }
        usleep(RETRY_MS * 1000);
    }
    mpp_packet_deinit(&packet);
    if (!pkt_done) { *out_data = NULL; *out_len = 0; return -1; }

    MppFrame frame = NULL;
    for (int tries = 0; tries < MAX_RETRY; tries++) {
        ret = d->mpi->decode_get_frame(d->ctx, &frame);
        if (ret == MPP_OK && frame) break;
        usleep(RETRY_MS * 1000);
    }
    if (!frame) { *out_data = NULL; *out_len = 0; return -1; }

    if (mpp_frame_get_info_change(frame)) {
        d->width      = mpp_frame_get_width(frame);
        d->height     = mpp_frame_get_height(frame);
        d->hor_stride = mpp_frame_get_hor_stride(frame);
        d->ver_stride = mpp_frame_get_ver_stride(frame);
        d->buf_size   = mpp_frame_get_buf_size(frame);

        fprintf(stderr, "Decoder info change: %dx%d stride=%dx%d buf=%zu\n",
                d->width, d->height, d->hor_stride, d->ver_stride, d->buf_size);

        if (d->frm_grp) mpp_buffer_group_put(d->frm_grp);
        ret = mpp_buffer_group_get_internal(&d->frm_grp, MPP_BUFFER_TYPE_DRM);
        if (ret)
            ret = mpp_buffer_group_get_internal(&d->frm_grp, MPP_BUFFER_TYPE_ION);
        mpp_buffer_group_limit_config(d->frm_grp, d->buf_size, 24);

        d->mpi->control(d->ctx, MPP_DEC_SET_EXT_BUF_GROUP, d->frm_grp);
        d->mpi->control(d->ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
        d->info_ready = 1;

        mpp_frame_deinit(&frame);
        *out_data = NULL; *out_len = 0;
        return 0;
    }

    if (!d->info_ready) {
        mpp_frame_deinit(&frame);
        *out_data = NULL; *out_len = 0;
        return 0;
    }

    MppBuffer buf = mpp_frame_get_buffer(frame);
    size_t frm_size = d->buf_size;
    uint8_t *copy = malloc(frm_size);
    memcpy(copy, mpp_buffer_get_ptr(buf), frm_size);
    *out_data = copy;
    *out_len  = frm_size;

    d->frame_count++;
    mpp_frame_deinit(&frame);
    return 0;
}

static void dec_deinit(Dec *d) {
    if (d->frm_grp) { mpp_buffer_group_put(d->frm_grp); d->frm_grp = NULL; }
    if (d->ctx)      { mpp_destroy(d->ctx); d->ctx = NULL; }
}

/* ── Main ──────────────────────────────────────────────────── */
static volatile int g_quit = 0;

static void sig_handler(int sig) {
    (void)sig;
    g_quit = 1;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [-d /dev/video0] [-n frames] [-o prefix]\n"
        "  -d  Video device     (default: /dev/video0)\n"
        "  -n  Number of frames (default: 300, 0=infinite)\n"
        "  -o  Output prefix    (default: ./output)\n",
        prog);
}

int main(int argc, char *argv[]) {
    const char *device  = "/dev/video0";
    int   max_frames    = 300;
    const char *prefix  = "./output";
    int   width         = 640;
    int   height        = 480;
    int   fps           = 30;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc) device = argv[++i];
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) max_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) prefix = argv[++i];
        else { usage(argv[0]); return 1; }
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    fprintf(stderr, "Opening camera: %s\n", device);
    CamSource *cam = camera_source_init(device, 4, width, height, MPP_FMT_YUV420SP);
    if (!cam) { fprintf(stderr, "Camera open failed\n"); return 1; }

    Enc enc = {0};
    int bps = width * height / 8 * fps;
    if (enc_init(&enc, width, height, fps, bps) != 0) {
        camera_source_deinit(cam); return 1;
    }

    Dec dec = {0};
    if (dec_init(&dec) != 0) {
        enc_deinit(&enc); camera_source_deinit(cam); return 1;
    }

    char fname[256];
    snprintf(fname, sizeof(fname), "%s.h264", prefix);
    FILE *fp_h264 = fopen(fname, "wb");
    snprintf(fname, sizeof(fname), "%s.yuv", prefix);
    FILE *fp_yuv  = fopen(fname, "wb");
    if (!fp_h264 || !fp_yuv) {
        fprintf(stderr, "Failed to open output files\n");
        dec_deinit(&dec); enc_deinit(&enc); camera_source_deinit(cam); return 1;
    }

    {
        uint8_t *hdr = NULL; size_t hlen = 0;
        if (enc_get_hdr(&enc, &hdr, &hlen) == 0 && hlen > 0) {
            fwrite(hdr, 1, hlen, fp_h264);
            fflush(fp_h264);
            fprintf(stderr, "SPS/PPS: %zu bytes\n", hlen);
            free(hdr);
        }
    }

    /* skip unstable initial frames */
    for (int i = 0; i < 10; i++) {
        int idx = camera_source_get_frame(cam);
        if (idx >= 0) camera_source_put_frame(cam, idx);
    }

    int stat_slots = (max_frames > 0) ? max_frames : 10000;
    stats_init(stat_slots);

    fprintf(stderr, "Capturing %d frames (Ctrl-C to stop)\n", max_frames);
    int frame_idx = 0;

    while (!g_quit) {
        if (max_frames > 0 && frame_idx >= max_frames) break;

        int cam_idx = camera_source_get_frame(cam);
        if (cam_idx < 0) { usleep(5000); continue; }
        int64_t ts_cap = now_ns();

        MppBuffer cam_buf = camera_frame_to_buf(cam, cam_idx);
        if (!cam_buf) { camera_source_put_frame(cam, cam_idx); continue; }

        uint8_t *enc_data = NULL; size_t enc_len = 0;
        if (enc_encode(&enc, cam_buf, &enc_data, &enc_len) != 0) {
            camera_source_put_frame(cam, cam_idx); continue;
        }
        int64_t ts_enc = now_ns();

        fwrite(enc_data, 1, enc_len, fp_h264);

        uint8_t *dec_data = NULL; size_t dec_len = 0;
        int is_last = (max_frames > 0 && frame_idx == max_frames - 1);
        int dec_ret = dec_decode(&dec, enc_data, enc_len,
                                 &dec_data, &dec_len, is_last);
        int64_t ts_dec = now_ns();

        if (dec_ret == 0 && dec_data && dec_len > 0) {
            fwrite(dec_data, 1, dec_len, fp_yuv);
        }

        stats_record(ts_cap, ts_enc, ts_dec, (int)enc_len);

        free(enc_data); free(dec_data);
        camera_source_put_frame(cam, cam_idx);
        frame_idx++;

        if (frame_idx % STATS_INTERVAL == 0) {
            stats_report_periodic(frame_idx - STATS_INTERVAL, frame_idx);
        }
    }

    fprintf(stderr, "Stopped after %d frames\n", frame_idx);

    /* EOS flush */
    {
        uint8_t *dummy = NULL; size_t dlen = 0;
        dec_decode(&dec, NULL, 0, &dummy, &dlen, 1);
        free(dummy);
    }

    if (g_stats.count > 0) {
        int rem = g_stats.count % STATS_INTERVAL;
        if (rem > 0)
            stats_report_periodic(g_stats.count - rem, g_stats.count);
        stats_report_final();
    }

    fclose(fp_h264);
    fclose(fp_yuv);
    dec_deinit(&dec);
    enc_deinit(&enc);
    camera_source_deinit(cam);
    free(g_stats.entries);

    fprintf(stderr, "Done. Output: %s.h264, %s.yuv\n", prefix, prefix);
    return 0;
}
