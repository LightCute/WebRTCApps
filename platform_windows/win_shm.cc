// win_shm.cc
#include "apps/platform_windows/win_shm.h"

#include "rtc_base/logging.h"

WinSharedMemory::WinSharedMemory() = default;

WinSharedMemory::~WinSharedMemory() { Close(); }

bool WinSharedMemory::Create(const char* name, size_t size) {
  // Create a named file mapping backed by system paging file (INVALID_HANDLE_VALUE).
  hMap_ = CreateFileMappingA(
      INVALID_HANDLE_VALUE,   // paging file
      nullptr,                 // default security
      PAGE_READWRITE,
      0,                       // high-order size (0 for full size)
      static_cast<DWORD>(size),
      name);                   // "webrtc_video_local" etc
  return hMap_ != nullptr;
}

bool WinSharedMemory::Open(const char* name) {
  hMap_ = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
  return hMap_ != nullptr;
}

void* WinSharedMemory::Map(size_t offset, size_t size) {
  if (!hMap_) return nullptr;
  mapped_ = MapViewOfFile(hMap_, FILE_MAP_ALL_ACCESS, 0,
                           static_cast<DWORD>(offset), size);
  mapped_size_ = size;
  return mapped_;
}

void WinSharedMemory::Unmap(void* ptr, size_t /*size*/) {
  if (ptr) {
    UnmapViewOfFile(ptr);
    mapped_ = nullptr;
  }
}

void WinSharedMemory::Close() {
  if (mapped_) UnmapViewOfFile(mapped_);
  if (hMap_) { CloseHandle(hMap_); hMap_ = nullptr; }
}

void WinSharedMemory::Lock() {}
void WinSharedMemory::Unlock() {}
void WinSharedMemory::WaitWritable() {}
void WinSharedMemory::SignalReadable() {}
void WinSharedMemory::WaitReadable() {}
void WinSharedMemory::SignalWritable() {}
