// video_frame_shm_ctrl.h — multi-consumer SHM control block for DMA-BUF video pipeline
#ifndef APPS_PEERCONNECTION_VIDEO_CAPTURE_SHM_RGA_VIDEO_FRAME_SHM_CTRL_H_
#define APPS_PEERCONNECTION_VIDEO_CAPTURE_SHM_RGA_VIDEO_FRAME_SHM_CTRL_H_

#include <pthread.h>
#include <string>

#include "apps/peerconnection/client_arm64/shm_common.h"

// ── Multi-consumer support ──
static constexpr int MAX_CONSUMERS = 4;
static constexpr int CONSUMER_FLAG_SKIP_ALLOWED = 0x01;

// ── Multi-consumer control block (placed in SysV SHM) ──
// Distinct type from shm_common.h's ShmCtrlBlock to avoid ODR violations
// when both headers are included in the same TU (RGA path + software fallback).
struct ShmMultiCtrlBlock {
  pthread_mutex_t mtx;
  pthread_cond_t  cv_can_write;
  pthread_cond_t  cv_can_read;

  uint32_t w_idx;                             // producer write cursor
  uint32_t r_idx[MAX_CONSUMERS];              // per-consumer read cursors
  uint32_t consumer_mask;                     // bit i set = consumer i is online
  uint32_t consumer_flags[MAX_CONSUMERS];     // per-consumer attribute flags

  uint32_t frame_count;                       // retained, unused in no-backpressure mode
  uint32_t seq[RING_BUFFER_CNT];              // seqlock per slot (odd=writing, even=done)

  RingVideoFrameItem ring[RING_BUFFER_CNT];
};

static constexpr size_t SHM_MULTI_CTRL_BLOCK_SIZE = sizeof(ShmMultiCtrlBlock);

// ── Socket handshake protocol ──
struct HandshakeMsg {
  uint32_t consumer_id;
  uint32_t frame_size;   // size of each dma-buf slot in bytes
};

struct RegistrationMsg {
  uint32_t flags;        // 0 or CONSUMER_FLAG_SKIP_ALLOWED
};

// ── Init (called by producer after shmat) ──
int video_frame_shm_init(ShmMultiCtrlBlock* shm);

#endif  // APPS_PEERCONNECTION_VIDEO_CAPTURE_SHM_RGA_VIDEO_FRAME_SHM_CTRL_H_
