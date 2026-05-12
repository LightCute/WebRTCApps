// rk_mpp_stubs.cc — Wrapper functions for MPP symbols resolved via dlopen.
// The real MPP functions are loaded at runtime via dlsym with RTLD_GLOBAL.
// These wrappers satisfy the linker at build time and delegate to dlsym'd
// function pointers at runtime.

#include <dlfcn.h>

// Include MPP headers for type definitions
extern "C" {
#include "mpp_buffer.h"
#include "mpp_err.h"
#include "mpp_frame.h"
#include "mpp_meta.h"
#include "mpp_packet.h"
#include "rk_mpi.h"
#include "rk_venc_cfg.h"
}

// Undefine macros that could interfere with our wrapper definitions
#undef mpp_buffer_get
#undef mpp_buffer_put
#undef mpp_buffer_get_ptr
#undef mpp_buffer_inc_ref
#undef mpp_buffer_commit
#undef mpp_buffer_import
#undef mpp_buffer_get_fd
#undef mpp_buffer_get_size
#undef mpp_buffer_get_index
#undef mpp_buffer_read
#undef mpp_buffer_write
#undef mpp_buffer_sync_begin
#undef mpp_buffer_sync_end
#undef mpp_buffer_group_get_internal
#undef mpp_buffer_group_get_external

// ── Function pointer table ──────────────────────────────
static struct {
  bool resolved = false;
  void* lib = nullptr;

  // rk_mpi.h
  MPP_RET (*create)(MppCtx*, MppApi**);
  MPP_RET (*init)(MppCtx, MppCtxType, MppCodingType);
  MPP_RET (*destroy)(MppCtx);

  // mpp_buffer.h
  MPP_RET (*buffer_group_get)(MppBufferGroup*, MppBufferType, MppBufferMode,
                              const char*, const char*);
  MPP_RET (*buffer_group_put)(MppBufferGroup);
  MPP_RET (*buffer_group_limit_config)(MppBufferGroup, size_t, RK_S32);
  MPP_RET (*buffer_get_with_tag)(MppBufferGroup, MppBuffer*, size_t,
                                 const char*, const char*);
  MPP_RET (*buffer_put_with_caller)(MppBuffer, const char*);
  void*   (*buffer_get_ptr_with_caller)(MppBuffer, const char*);

  // rk_venc_cfg.h
  MPP_RET (*enc_cfg_init)(MppEncCfg*);
  MPP_RET (*enc_cfg_set_s32)(MppEncCfg, const char*, RK_S32);
  MPP_RET (*enc_cfg_deinit)(MppEncCfg);

  // mpp_packet.h
  MPP_RET (*packet_init)(MppPacket*, void*, size_t);
  MPP_RET (*packet_init_with_buffer)(MppPacket*, MppBuffer);
  MPP_RET (*packet_deinit)(MppPacket*);
  void*   (*packet_get_pos)(const MppPacket);
  size_t  (*packet_get_length)(const MppPacket);
  void    (*packet_set_length)(MppPacket, size_t);
  MPP_RET (*packet_set_eos)(MppPacket);

  // mpp_frame.h
  MPP_RET (*frame_init)(MppFrame*);
  MPP_RET (*frame_deinit)(MppFrame*);
  void    (*frame_set_width)(MppFrame, RK_U32);
  void    (*frame_set_height)(MppFrame, RK_U32);
  void    (*frame_set_hor_stride)(MppFrame, RK_U32);
  void    (*frame_set_ver_stride)(MppFrame, RK_U32);
  void    (*frame_set_fmt)(MppFrame, MppFrameFormat);
  void    (*frame_set_buffer)(MppFrame, MppBuffer);
  void    (*frame_set_eos)(MppFrame, RK_U32);
  MppBuffer (*frame_get_buffer)(const MppFrame);
  MppMeta  (*frame_get_meta)(const MppFrame);
  RK_U32  (*frame_get_info_change)(const MppFrame);
  RK_U32  (*frame_get_width)(const MppFrame);
  RK_U32  (*frame_get_height)(const MppFrame);
  RK_U32  (*frame_get_hor_stride)(const MppFrame);
  RK_U32  (*frame_get_ver_stride)(const MppFrame);
  size_t  (*frame_get_buf_size)(const MppFrame);

  // mpp_meta.h
  MPP_RET (*meta_set_packet)(MppMeta, MppMetaKey, MppPacket);
} mpp;

// ── Lazy resolution ─────────────────────────────────────
// Called once before any MPP function use. Returns true if the
// real librockchip_mpp.so was loaded and all symbols resolved.

bool MppLibLoad() {
  if (mpp.resolved) return true;

  mpp.lib = dlopen("librockchip_mpp.so.1", RTLD_NOW | RTLD_GLOBAL);
  if (!mpp.lib) {
    mpp.lib = dlopen("librockchip_mpp.so", RTLD_NOW | RTLD_GLOBAL);
  }
  if (!mpp.lib) return false;

  #define LOAD(name)                                                    \
    mpp.name = reinterpret_cast<decltype(mpp.name)>(dlsym(mpp.lib, "mpp_" #name)); \
    if (!mpp.name) { dlclose(mpp.lib); mpp.lib = nullptr; return false; }

  LOAD(create);
  LOAD(init);
  LOAD(destroy);
  LOAD(buffer_group_get);
  LOAD(buffer_group_put);
  LOAD(buffer_group_limit_config);
  LOAD(buffer_get_with_tag);
  LOAD(buffer_put_with_caller);
  LOAD(buffer_get_ptr_with_caller);
  LOAD(enc_cfg_init);
  LOAD(enc_cfg_set_s32);
  LOAD(enc_cfg_deinit);
  LOAD(packet_init);
  LOAD(packet_init_with_buffer);
  LOAD(packet_deinit);
  LOAD(packet_get_pos);
  LOAD(packet_get_length);
  LOAD(packet_set_length);
  LOAD(packet_set_eos);
  LOAD(frame_init);
  LOAD(frame_deinit);
  LOAD(frame_set_width);
  LOAD(frame_set_height);
  LOAD(frame_set_hor_stride);
  LOAD(frame_set_ver_stride);
  LOAD(frame_set_fmt);
  LOAD(frame_set_buffer);
  LOAD(frame_set_eos);
  LOAD(frame_get_buffer);
  LOAD(frame_get_info_change);
  LOAD(frame_get_width);
  LOAD(frame_get_height);
  LOAD(frame_get_hor_stride);
  LOAD(frame_get_ver_stride);
  LOAD(frame_get_buf_size);
  LOAD(frame_get_meta);
  LOAD(meta_set_packet);

  #undef LOAD

  mpp.resolved = true;
  return true;
}

// ── Wrapper functions (extern "C" — match MPP SDK declarations) ──
extern "C" {

MPP_RET mpp_create(MppCtx* ctx, MppApi** mpi_result) {
  return mpp.create(ctx, mpi_result);
}

MPP_RET mpp_init(MppCtx ctx, MppCtxType type, MppCodingType coding) {
  return mpp.init(ctx, type, coding);
}

MPP_RET mpp_destroy(MppCtx ctx) {
  return mpp.destroy(ctx);
}

MPP_RET mpp_buffer_group_get(MppBufferGroup* group, MppBufferType type,
                             MppBufferMode mode, const char* tag, const char* caller) {
  return mpp.buffer_group_get(group, type, mode, tag, caller);
}

MPP_RET mpp_buffer_group_put(MppBufferGroup group) {
  return mpp.buffer_group_put(group);
}

MPP_RET mpp_buffer_group_limit_config(MppBufferGroup group, size_t size, RK_S32 count) {
  return mpp.buffer_group_limit_config(group, size, count);
}

MPP_RET mpp_buffer_get_with_tag(MppBufferGroup group, MppBuffer* buffer, size_t size,
                                const char* tag, const char* caller) {
  return mpp.buffer_get_with_tag(group, buffer, size, tag, caller);
}

MPP_RET mpp_buffer_put_with_caller(MppBuffer buffer, const char* caller) {
  return mpp.buffer_put_with_caller(buffer, caller);
}

void* mpp_buffer_get_ptr_with_caller(MppBuffer buffer, const char* caller) {
  return mpp.buffer_get_ptr_with_caller(buffer, caller);
}

MPP_RET mpp_enc_cfg_init(MppEncCfg* cfg) {
  return mpp.enc_cfg_init(cfg);
}

MPP_RET mpp_enc_cfg_set_s32(MppEncCfg cfg, const char* name, RK_S32 val) {
  return mpp.enc_cfg_set_s32(cfg, name, val);
}

MPP_RET mpp_enc_cfg_deinit(MppEncCfg cfg) {
  return mpp.enc_cfg_deinit(cfg);
}

MPP_RET mpp_packet_init(MppPacket* packet, void* data, size_t size) {
  return mpp.packet_init(packet, data, size);
}

MPP_RET mpp_packet_deinit(MppPacket* packet) {
  return mpp.packet_deinit(packet);
}

void* mpp_packet_get_pos(const MppPacket packet) {
  return mpp.packet_get_pos(packet);
}

size_t mpp_packet_get_length(const MppPacket packet) {
  return mpp.packet_get_length(packet);
}

void mpp_packet_set_length(MppPacket packet, size_t size) {
  mpp.packet_set_length(packet, size);
}

MPP_RET mpp_packet_set_eos(MppPacket packet) {
  return mpp.packet_set_eos(packet);
}

MPP_RET mpp_packet_init_with_buffer(MppPacket* packet, MppBuffer buffer) {
  return mpp.packet_init_with_buffer(packet, buffer);
}

MPP_RET mpp_frame_init(MppFrame* frame) {
  return mpp.frame_init(frame);
}

MPP_RET mpp_frame_deinit(MppFrame* frame) {
  return mpp.frame_deinit(frame);
}

void mpp_frame_set_width(MppFrame frame, RK_U32 width) {
  mpp.frame_set_width(frame, width);
}

void mpp_frame_set_height(MppFrame frame, RK_U32 height) {
  mpp.frame_set_height(frame, height);
}

void mpp_frame_set_hor_stride(MppFrame frame, RK_U32 stride) {
  mpp.frame_set_hor_stride(frame, stride);
}

void mpp_frame_set_ver_stride(MppFrame frame, RK_U32 stride) {
  mpp.frame_set_ver_stride(frame, stride);
}

void mpp_frame_set_fmt(MppFrame frame, MppFrameFormat fmt) {
  mpp.frame_set_fmt(frame, fmt);
}

void mpp_frame_set_buffer(MppFrame frame, MppBuffer buffer) {
  mpp.frame_set_buffer(frame, buffer);
}

void mpp_frame_set_eos(MppFrame frame, RK_U32 eos) {
  mpp.frame_set_eos(frame, eos);
}

MppBuffer mpp_frame_get_buffer(const MppFrame frame) {
  return mpp.frame_get_buffer(frame);
}

RK_U32 mpp_frame_get_info_change(const MppFrame frame) {
  return mpp.frame_get_info_change(frame);
}

RK_U32 mpp_frame_get_width(const MppFrame frame) {
  return mpp.frame_get_width(frame);
}

RK_U32 mpp_frame_get_height(const MppFrame frame) {
  return mpp.frame_get_height(frame);
}

RK_U32 mpp_frame_get_hor_stride(const MppFrame frame) {
  return mpp.frame_get_hor_stride(frame);
}

RK_U32 mpp_frame_get_ver_stride(const MppFrame frame) {
  return mpp.frame_get_ver_stride(frame);
}

size_t mpp_frame_get_buf_size(const MppFrame frame) {
  return mpp.frame_get_buf_size(frame);
}

MppMeta mpp_frame_get_meta(const MppFrame frame) {
  return mpp.frame_get_meta(frame);
}

MPP_RET mpp_meta_set_packet(MppMeta meta, MppMetaKey key, MppPacket packet) {
  return mpp.meta_set_packet(meta, key, packet);
}

}  // extern "C"
