// posix_shm.cc
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunsafe-buffer-usage"
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"

#include "apps/client_x64_linux/posix_shm.h"

#include <sys/ipc.h>
#include <sys/shm.h>
#include <cstring>

#include "rtc_base/logging.h"

PosixSharedMemory::PosixSharedMemory() = default;
PosixSharedMemory::~PosixSharedMemory() { Close(); }

bool PosixSharedMemory::Create(const char* name, size_t size) {
  key_ = ftok(name, 0x88);
  if (key_ == -1) return false;
  shmid_ = shmget(key_, size, IPC_CREAT | 0666);
  return shmid_ >= 0;
}

bool PosixSharedMemory::Open(const char* name) {
  key_ = ftok(name, 0x88);
  if (key_ == -1) return false;
  shmid_ = shmget(key_, 0, 0666);
  return shmid_ >= 0;
}

void* PosixSharedMemory::Map(size_t offset, size_t size) {
  if (shmid_ < 0) return nullptr;
  void* ptr = shmat(shmid_, nullptr, 0);
  if (ptr == (void*)-1) return nullptr;
  return static_cast<char*>(ptr) + offset;
}

void PosixSharedMemory::Unmap(void* ptr, size_t /*size*/) {
  if (ptr) shmdt(ptr);
}

void PosixSharedMemory::Close() {
  if (shmid_ >= 0) {
    shmctl(shmid_, IPC_RMID, nullptr);
    shmid_ = -1;
  }
}

// The sync primitives are stored at the start of the shared memory block
// by the ring-buffer protocol layer (ShmVideoWriter/ShmVideoReader).
// These methods are no-ops at this level — the sync is managed by
// ShmVideoWriter/ShmVideoReader via the control block.

void PosixSharedMemory::Lock() {}
void PosixSharedMemory::Unlock() {}
void PosixSharedMemory::WaitWritable() {}
void PosixSharedMemory::SignalReadable() {}
void PosixSharedMemory::WaitReadable() {}
void PosixSharedMemory::SignalWritable() {}
#pragma GCC diagnostic pop
