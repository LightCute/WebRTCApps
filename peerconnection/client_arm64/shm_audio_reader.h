// shm_audio_reader.h
#pragma once
#include "shm_common.h"
#include <atomic>
#include <string>

class ShmAudioReader {
 public:
  ShmAudioReader();
  ~ShmAudioReader();

  ShmAudioReader(const ShmAudioReader&) = delete;
  ShmAudioReader& operator=(const ShmAudioReader&) = delete;

  bool Init(const std::string& key_path, int proj_id);
  bool ReadFrame(AudioFrameHead& out_head, uint8_t* out_data, uint32_t buf_size);
  void RequestStop();

 private:
  int32_t m_shmid{-1};
  ShmAudioCtrlBlock* m_shm_ptr{nullptr};
  bool m_inited{false};
  std::atomic<bool> stop_requested_{false};
};
