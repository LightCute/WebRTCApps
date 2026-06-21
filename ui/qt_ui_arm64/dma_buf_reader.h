// dma_buf_reader.h — multi-consumer DMA-BUF zero-copy reader
#pragma once

#include "shm_common.h"
#include <atomic>
#include <string>
#include <vector>

struct DmaBufSlot {
  int fd = -1;
  void* ptr = nullptr;
  size_t size = 0;
};

class DmaBufReader {
 public:
  static constexpr int kNumSlots = 8;

  DmaBufReader();
  ~DmaBufReader();

  // Connect to producer: attach to control block SHM, receive dma-buf fds,
  // perform handshake (send RegistrationMsg, receive HandshakeMsg).
  // flags: 0 or CONSUMER_FLAG_SKIP_ALLOWED
  int Init(const std::string& ctrl_shm_path, int ctrl_proj_id,
           const std::string& socket_path, uint32_t flags);

  // Get next frame with seqlock-protected read. Returns true on success.
  bool ReadFrame(VideoFrameHead& out_head, const uint8_t*& out_data);

  // Skip to the latest frame (producer w_idx). For consumers that
  // process frames at irregular intervals (e.g., AI inference).
  void SkipToLatest();

  void RequestStop();

 private:
  int shmid_ = -1;
  ShmMultiCtrlBlock* ctrl_ = nullptr;
  DmaBufSlot slots_[8];
  int consumer_id_ = -1;
  int sock_fd_ = -1;         // keep-alive: producer detects disconnect via socket close
  uint32_t r_idx_ = 0;
  uint32_t flags_ = 0;
  bool inited_ = false;
  std::atomic<bool> stop_requested_{false};
};
