// dma_buf_pool.cc — DMA-BUF allocation via librga (adapts to kernel allocator)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunsafe-buffer-usage"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"

#include "apps/peerconnection/video_capture_shm_RGA/dma_buf_pool.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>

// RGA types needed for bo_t (avoid RgaApi.h — its <linux/stddef.h> breaks libc)
extern "C" {
#include "rga.h"
#include "drmrga.h"
}

DmaBufPool::DmaBufPool() {
  for (int i = 0; i < kNumSlots; ++i) {
    slots_[i].fd = -1;
    slots_[i].ptr = nullptr;
    slots_[i].size = 0;
  }
}

DmaBufPool::~DmaBufPool() {
  for (int i = 0; i < kNumSlots; ++i) {
    if (slots_[i].ptr && slots_[i].ptr != MAP_FAILED) {
      munmap(slots_[i].ptr, slots_[i].size);
    }
    if (slots_[i].fd >= 0) {
      close(slots_[i].fd);
    }
  }
}

int DmaBufPool::Allocate(size_t frame_size) {
  // Try librga allocation first (handles ION/DRM/dma-heap transparently)
  void* lib = dlopen("librga.so", RTLD_NOW | RTLD_GLOBAL);
  if (!lib) {
    fprintf(stderr, "DmaBufPool: dlopen librga.so failed: %s\n", dlerror());
    goto fallback_dma_heap;
  }

  {
    auto fn_alloc = reinterpret_cast<int(*)(bo_t*, int, int, int)>(
        dlsym(lib, "c_RkRgaGetAllocBuffer"));
    auto fn_get_fd = reinterpret_cast<int(*)(bo_t*, int*)>(
        dlsym(lib, "c_RkRgaGetBufferFd"));
    auto fn_mmap = reinterpret_cast<int(*)(bo_t*)>(
        dlsym(lib, "c_RkRgaGetMmap"));
    auto fn_free = reinterpret_cast<int(*)(bo_t*)>(
        dlsym(lib, "c_RkRgaFree"));
    auto fn_unmap = reinterpret_cast<int(*)(bo_t*)>(
        dlsym(lib, "c_RkRgaUnmap"));

    if (fn_alloc && fn_get_fd && fn_mmap) {
      // bpp=12 for I420 packed (8 bits Y + 2 bits U + 2 bits V per pixel avg)
      const int bpp = 12;
      const int w = 640, h = 480;
      bool ok = true;

      for (int i = 0; i < kNumSlots && ok; ++i) {
        bo_t bo;
        memset(&bo, 0, sizeof(bo));
        if (fn_alloc(&bo, w, h, bpp) != 0) {
          fprintf(stderr, "DmaBufPool: c_RkRgaGetAllocBuffer[%d] failed\n", i);
          ok = false;
          break;
        }

        int fd = -1;
        if (fn_get_fd(&bo, &fd) != 0 || fd < 0) {
          fprintf(stderr, "DmaBufPool: c_RkRgaGetBufferFd[%d] failed\n", i);
          fn_free(&bo);
          ok = false;
          break;
        }

        if (fn_mmap(&bo) != 0) {
          fprintf(stderr, "DmaBufPool: c_RkRgaGetMmap[%d] failed\n", i);
          close(fd);
          fn_free(&bo);
          ok = false;
          break;
        }

        slots_[i].fd = fd;
        slots_[i].ptr = bo.ptr;
        slots_[i].size = bo.size;
      }

      if (ok) {
        fprintf(stderr, "DmaBufPool: allocated %d buffers via librga (%zu bytes each)\n",
                kNumSlots, slots_[0].size);
        return 0;
      }

      // Clean up partially-allocated slots
      for (int j = kNumSlots - 1; j >= 0; --j) {
        if (slots_[j].fd < 0) continue;
        bo_t bo;
        memset(&bo, 0, sizeof(bo));
        bo.fd = slots_[j].fd;
        bo.ptr = slots_[j].ptr;
        bo.size = slots_[j].size;
        fn_unmap(&bo);
        fn_free(&bo);
        close(slots_[j].fd);
        slots_[j].fd = -1;
        slots_[j].ptr = nullptr;
      }
      // Fall through to dma_heap
    }
  }

fallback_dma_heap:
  // Try /dev/dma_heap/linux,cma as fallback
  fprintf(stderr, "DmaBufPool: librga alloc failed, trying dma_heap...\n");

  int heap_fd = open("/dev/dma_heap/linux,cma", O_RDONLY | O_CLOEXEC);
  if (heap_fd < 0) {
    heap_fd = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
  }
  if (heap_fd < 0) {
    fprintf(stderr, "DmaBufPool: dma_heap also unavailable: %s\n", strerror(errno));
    return -errno;
  }

  for (int i = 0; i < kNumSlots; ++i) {
    struct { uint64_t len; uint32_t fd; uint32_t fd_flags; uint64_t heap_flags; } alloc = {};
    alloc.len = frame_size;
    alloc.fd_flags = O_RDWR | O_CLOEXEC;
    if (ioctl(heap_fd, 0xc0184800 /* DMA_HEAP_IOCTL_ALLOC */, &alloc) < 0) {
      fprintf(stderr, "DmaBufPool: DMA_HEAP_IOCTL_ALLOC[%d] failed: %s\n",
              i, strerror(errno));
      close(heap_fd);
      return -errno;
    }

    void* ptr = mmap(nullptr, frame_size, PROT_READ | PROT_WRITE,
                     MAP_SHARED, (int)alloc.fd, 0);
    if (ptr == MAP_FAILED) {
      close((int)alloc.fd);
      close(heap_fd);
      return -errno;
    }
    slots_[i].fd = (int)alloc.fd;
    slots_[i].ptr = ptr;
    slots_[i].size = frame_size;
  }
  close(heap_fd);
  return 0;
}

bool DmaBufPool::ImportFd(int index, int fd, size_t size) {
  if (index < 0 || index >= kNumSlots || fd < 0) return false;

  int dup_fd = dup(fd);
  if (dup_fd < 0) {
    perror("DmaBufPool::ImportFd: dup");
    return false;
  }

  void* ptr = mmap(nullptr, size, PROT_READ, MAP_SHARED, dup_fd, 0);
  if (ptr == MAP_FAILED) {
    perror("DmaBufPool::ImportFd: mmap");
    close(dup_fd);
    return false;
  }

  slots_[index].fd = dup_fd;
  slots_[index].ptr = ptr;
  slots_[index].size = size;
  return true;
}

#pragma clang diagnostic pop
#pragma GCC diagnostic pop
