#include "shm_reader.h"
#include <sys/shm.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <ctime>

ShmVideoReader::ShmVideoReader() = default;

ShmVideoReader::~ShmVideoReader() {
  if (m_shm_ptr != nullptr) {
    shmdt(m_shm_ptr);
  }
}

bool ShmVideoReader::init(const std::string& key_path, int proj_id) {
  // Ensure the key file exists (may be created by writer)
  int fd = open(key_path.c_str(), O_CREAT | O_WRONLY, 0666);
  if (fd != -1) close(fd);

  key_t key = ftok(key_path.c_str(), proj_id);
  if (key == -1) {
    perror("ftok");
    return false;
  }

  m_shmid = shmget(key, SHM_CTRL_BLOCK_SIZE, 0666);
  if (m_shmid == -1) {
    perror("shmget");
    return false;
  }

  m_shm_ptr = static_cast<ShmCtrlBlock*>(shmat(m_shmid, nullptr, 0));
  if (m_shm_ptr == reinterpret_cast<void*>(-1)) {
    perror("shmat");
    m_shm_ptr = nullptr;
    return false;
  }

  m_inited = true;
  return true;
}

bool ShmVideoReader::read_frame(VideoFrameHead& out_head,
                                uint8_t* out_data,
                                uint32_t buf_size) {
  if (!m_inited || !out_data) return false;

  pthread_mutex_lock(&m_shm_ptr->mtx);

  while (m_shm_ptr->frame_count == 0) {
    pthread_cond_wait(&m_shm_ptr->cv_can_read, &m_shm_ptr->mtx);
  }

  RingFrameItem& item = m_shm_ptr->ring[m_shm_ptr->r_idx];
  out_head = item.head;

  if (buf_size < out_head.frame_len) {
    pthread_mutex_unlock(&m_shm_ptr->mtx);
    return false;
  }

  memcpy(out_data, item.data, out_head.frame_len);

  m_shm_ptr->r_idx = (m_shm_ptr->r_idx + 1) % RING_BUFFER_CNT;
  m_shm_ptr->frame_count--;

  pthread_cond_signal(&m_shm_ptr->cv_can_write);
  pthread_mutex_unlock(&m_shm_ptr->mtx);
  return true;
}
