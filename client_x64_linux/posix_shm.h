// posix_shm.h — POSIX shared memory + pthread sync for Linux.
#ifndef APPS_CLIENT_X64_LINUX_POSIX_SHM_H_
#define APPS_CLIENT_X64_LINUX_POSIX_SHM_H_

#include <pthread.h>
#include <sys/types.h>

#include "apps/webrtc_engine/shared_memory_interface.h"

class PosixSharedMemory : public ISharedMemory {
 public:
  PosixSharedMemory();
  ~PosixSharedMemory() override;

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
  int shmid_ = -1;
  key_t key_ = 0;
};

#endif  // APPS_CLIENT_X64_LINUX_POSIX_SHM_H_
