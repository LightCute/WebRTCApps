// dma_buf_pool.h — CMA dma-buf ring buffer pool (shared by producer & consumer)
#ifndef APPS_PEERCONNECTION_APPS_PEERCONNECTION_CLIENT_DMA_BUF_POOL_H_
#define APPS_PEERCONNECTION_APPS_PEERCONNECTION_CLIENT_DMA_BUF_POOL_H_

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunsafe-buffer-usage"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct DmaBufSlot {
  int fd = -1;           // dma-buf fd
  void* ptr = nullptr;   // mmap'd userspace pointer
  size_t size = 0;       // buffer size in bytes
};

// Pool of CMA-allocated dma-buf slots for zero-copy frame exchange.
//
// Producer: calls Allocate() → uses slots[idx].fd as RGA destination.
// Consumer: receives fds via Unix socket → calls ImportFd() → uses slots[idx].ptr to read.
//
// The actual allocation uses /dev/dma_heap/linux,cma (CMA physically-contiguous
// memory required by RGA DMA engine).
class DmaBufPool {
 public:
  static constexpr int kNumSlots = 8;  // ring buffer depth

  DmaBufPool();
  ~DmaBufPool();

  // Allocate kNumSlots CMA buffers of frame_size bytes each.
  // Returns 0 on success, -errno on failure.
  int Allocate(size_t frame_size);

  // Import a single dma-buf fd into a slot (consumer side).
  // The fd is duplicated; caller still owns the original.
  bool ImportFd(int index, int fd, size_t size);

  // Export the fd for a slot (for passing over Unix socket).
  int GetFd(int index) const { return slots_[index].fd; }

  // Get mmap'd pointer for a slot (consumer reads from this).
  void* GetPtr(int index) const { return slots_[index].ptr; }

  size_t GetSlotSize(int index) const { return slots_[index].size; }

 private:
  DmaBufSlot slots_[kNumSlots];
};

#pragma clang diagnostic pop
#pragma GCC diagnostic pop

#endif  // APPS_PEERCONNECTION_APPS_PEERCONNECTION_CLIENT_DMA_BUF_POOL_H_
