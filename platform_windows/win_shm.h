// win_shm.h — Windows shared memory via CreateFileMapping / MapViewOfFile.
#ifndef APPS_PLATFORM_WINDOWS_WIN_SHM_H_
#define APPS_PLATFORM_WINDOWS_WIN_SHM_H_

#include <windows.h>

#include "apps/webrtc_engine/shared_memory_interface.h"

class WinSharedMemory : public ISharedMemory {
 public:
  WinSharedMemory();
  ~WinSharedMemory() override;

  bool Create(const char* name, size_t size) override;
  bool Open(const char* name) override;
  void* Map(size_t offset, size_t size) override;
  void Unmap(void* ptr, size_t size) override;
  void Close() override;

  void Lock() override;
  void Unlock() override;
  void WaitWritable() override;
  void SignalReadable() override;
  void WaitReadable() override;
  void SignalWritable() override;

 private:
  HANDLE hMap_ = nullptr;
  void* mapped_ = nullptr;
  size_t mapped_size_ = 0;
};

#endif  // APPS_PLATFORM_WINDOWS_WIN_SHM_H_
