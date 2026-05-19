#ifndef APPS_PEERCONNECTION_VIDEO_CAPTURE_SHM_MPP_DEC_SHM_VIDEO_READER_H_
#define APPS_PEERCONNECTION_VIDEO_CAPTURE_SHM_MPP_DEC_SHM_VIDEO_READER_H_

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

struct ShmCtrlBlock;
struct VideoFrameHead;

// Reads video frames from a POSIX shared-memory ring buffer.
// Mirrors ShmVideoWriter: uses the same ShmCtrlBlock layout, pthread
// synchronization, and ftok-based key derivation.
class ShmVideoReader {
 public:
  ShmVideoReader();
  ~ShmVideoReader();

  ShmVideoReader(const ShmVideoReader&) = delete;
  ShmVideoReader& operator=(const ShmVideoReader&) = delete;

  bool Init(const std::string& key_path, int proj_id);
  bool ReadFrame(VideoFrameHead* head, std::vector<uint8_t>* data);  // blocking
  void Stop();

 private:
  int32_t m_shmid{-1};
  ShmCtrlBlock* m_shm_ptr{nullptr};
  bool m_inited{false};
  std::atomic<bool> m_stopped{false};
};

#endif  // APPS_PEERCONNECTION_VIDEO_CAPTURE_SHM_MPP_DEC_SHM_VIDEO_READER_H_
