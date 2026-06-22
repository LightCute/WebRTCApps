// shared_memory_interface.h — cross-platform shared memory abstraction.
//
// Implementations:
//   Linux   — PosixSharedMemory  (shmget/shmat)
//   Windows — WinSharedMemory    (CreateFileMapping)
#ifndef APPS_WEBRTC_ENGINE_SHARED_MEMORY_INTERFACE_H_
#define APPS_WEBRTC_ENGINE_SHARED_MEMORY_INTERFACE_H_

#include <cstddef>

class ISharedMemory {
 public:
  virtual ~ISharedMemory() = default;

  // Create (server side): allocate a named shared memory region of `size` bytes.
  virtual bool Create(const char* name, size_t size) = 0;

  // Open (client side): open an existing shared memory region.
  virtual bool Open(const char* name) = 0;

  // Map into process address space. Returns pointer or nullptr on failure.
  virtual void* Map(size_t offset, size_t size) = 0;

  // Unmap.
  virtual void Unmap(void* ptr, size_t size) = 0;

  // Close the shared memory handle.
  virtual void Close() = 0;

  // Synchronisation primitives — exposed for ring-buffer protocol.
  virtual void Lock() = 0;
  virtual void Unlock() = 0;
  virtual void WaitWritable() = 0;
  virtual void SignalReadable() = 0;
  virtual void WaitReadable() = 0;
  virtual void SignalWritable() = 0;
};

#endif  // APPS_WEBRTC_ENGINE_SHARED_MEMORY_INTERFACE_H_
