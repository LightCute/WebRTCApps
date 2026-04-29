#include "shm_writer.h"
#include <sys/shm.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunsafe-buffer-usage"
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"

ShmVideoWriter::ShmVideoWriter() = default;

ShmVideoWriter::~ShmVideoWriter() {
  if (m_shm_ptr != nullptr) {
    shmdt(m_shm_ptr);
  }
}

bool ShmVideoWriter::init(const std::string& key_path, int proj_id) {
  // ftok() requires the file to exist; create it if missing
  int fd = open(key_path.c_str(), O_CREAT | O_WRONLY, 0666);
  if (fd == -1) {
    perror("shm_writer: open key file");
    return false;
  }
  close(fd);

  key_t key = ftok(key_path.c_str(), proj_id);
  if (key == -1) {
    perror("shm_writer: ftok");
    return false;
  }

  m_shmid = shmget(key, SHM_CTRL_BLOCK_SIZE, IPC_CREAT | 0666);
  if (m_shmid == -1) return false;

  m_shm_ptr = static_cast<ShmCtrlBlock*>(shmat(m_shmid, nullptr, 0));
  if (m_shm_ptr == reinterpret_cast<void*>(-1)) {
    m_shm_ptr = nullptr;
    return false;
  }

  init_shm_sync(m_shm_ptr);
  m_inited = true;
  return true;
}

bool ShmVideoWriter::write_frame(const VideoFrameHead& head, const uint8_t* data) {
  if (!m_inited || !data) return false;

  pthread_mutex_lock(&m_shm_ptr->mtx);

  while (m_shm_ptr->frame_count >= RING_BUFFER_CNT) {
    pthread_cond_wait(&m_shm_ptr->cv_can_write, &m_shm_ptr->mtx);
  }

  RingFrameItem& item = m_shm_ptr->ring[m_shm_ptr->w_idx];
  item.head = head;
  memcpy(item.data, data, head.frame_len);

  m_shm_ptr->w_idx = (m_shm_ptr->w_idx + 1) % RING_BUFFER_CNT;
  m_shm_ptr->frame_count++;

  pthread_cond_signal(&m_shm_ptr->cv_can_read);
  pthread_mutex_unlock(&m_shm_ptr->mtx);
  return true;
}

#pragma GCC diagnostic pop
