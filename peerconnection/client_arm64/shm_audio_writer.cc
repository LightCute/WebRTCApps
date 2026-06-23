// shm_audio_writer.cc
#include "shm_audio_writer.h"
#include <sys/shm.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>

ShmAudioWriter::ShmAudioWriter() = default;

ShmAudioWriter::~ShmAudioWriter() {
  if (m_shm_ptr != nullptr) {
    shmdt(m_shm_ptr);
  }
}

bool ShmAudioWriter::Init(const std::string& key_path, int proj_id) {
  int fd = open(key_path.c_str(), O_CREAT | O_WRONLY, 0666);
  if (fd == -1) {
    perror("shm_audio_writer: open key file");
    return false;
  }
  close(fd);

  key_t key = ftok(key_path.c_str(), proj_id);
  if (key == -1) {
    perror("shm_audio_writer: ftok");
    return false;
  }

  m_shmid = shmget(key, SHM_AUDIO_CTRL_BLOCK_SIZE, IPC_CREAT | 0666);
  if (m_shmid == -1) {
    perror("shm_audio_writer: shmget");
    return false;
  }

  m_shm_ptr = static_cast<ShmAudioCtrlBlock*>(shmat(m_shmid, nullptr, 0));
  if (m_shm_ptr == reinterpret_cast<void*>(-1)) {
    perror("shm_audio_writer: shmat");
    m_shm_ptr = nullptr;
    return false;
  }

  if (init_shm_audio_sync(m_shm_ptr) != 0) {
    perror("shm_audio_writer: init_shm_audio_sync");
    return false;
  }
  m_inited = true;
  return true;
}

bool ShmAudioWriter::WriteFrame(const AudioFrameHead& head, const uint8_t* data) {
  if (!m_inited || !data) return false;
  if (head.frame_len > AUDIO_FRAME_MAX_SIZE) return false;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunsafe-buffer-usage"
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"

  pthread_mutex_lock(&m_shm_ptr->mtx);

  while (m_shm_ptr->frame_count >= AUDIO_RING_BUFFER_CNT) {
    if (pthread_cond_wait(&m_shm_ptr->cv_can_write, &m_shm_ptr->mtx) != 0) {
      pthread_mutex_unlock(&m_shm_ptr->mtx);
      return false;
    }
  }

  RingAudioFrameItem& item = m_shm_ptr->ring[m_shm_ptr->w_idx];
  item.head = head;
  memcpy(item.data, data, head.frame_len);

  m_shm_ptr->w_idx = (m_shm_ptr->w_idx + 1) % AUDIO_RING_BUFFER_CNT;
  m_shm_ptr->frame_count++;

  pthread_cond_signal(&m_shm_ptr->cv_can_read);
  pthread_mutex_unlock(&m_shm_ptr->mtx);
  return true;

#pragma GCC diagnostic pop
}
