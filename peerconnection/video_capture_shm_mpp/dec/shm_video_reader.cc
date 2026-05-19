#include "apps/peerconnection/video_capture_shm_mpp/dec/shm_video_reader.h"

#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>

#include "apps/peerconnection/client/shm_common.h"

ShmVideoReader::ShmVideoReader() = default;

ShmVideoReader::~ShmVideoReader() {
  if (m_shm_ptr != nullptr) {
    shmdt(m_shm_ptr);
  }
}

bool ShmVideoReader::Init(const std::string& key_path, int proj_id) {
  int fd = open(key_path.c_str(), O_RDONLY);
  if (fd == -1) {
    perror("shm_video_reader: open key file");
    return false;
  }
  close(fd);

  key_t key = ftok(key_path.c_str(), proj_id);
  if (key == -1) {
    perror("shm_video_reader: ftok");
    return false;
  }

  m_shmid = shmget(key, SHM_CTRL_BLOCK_SIZE, 0666);
  if (m_shmid == -1) {
    perror("shm_video_reader: shmget");
    return false;
  }

  m_shm_ptr = static_cast<ShmCtrlBlock*>(shmat(m_shmid, nullptr, 0));
  if (m_shm_ptr == reinterpret_cast<void*>(-1)) {
    perror("shm_video_reader: shmat");
    m_shm_ptr = nullptr;
    return false;
  }

  m_inited = true;
  return true;
}

bool ShmVideoReader::ReadFrame(VideoFrameHead* head,
                                std::vector<uint8_t>* data) {
  if (!m_inited || !head || !data) return false;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunsafe-buffer-usage"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"

  pthread_mutex_lock(&m_shm_ptr->mtx);

  while (m_shm_ptr->frame_count == 0 && !m_stopped) {
    pthread_cond_wait(&m_shm_ptr->cv_can_read, &m_shm_ptr->mtx);
  }
  if (m_stopped) {
    pthread_mutex_unlock(&m_shm_ptr->mtx);
    return false;
  }

  RingVideoFrameItem& item = m_shm_ptr->ring[m_shm_ptr->r_idx];
  *head = item.head;
  data->resize(item.head.frame_len);
  memcpy(data->data(), item.data, item.head.frame_len);

  m_shm_ptr->r_idx = (m_shm_ptr->r_idx + 1) % RING_BUFFER_CNT;
  m_shm_ptr->frame_count--;

  pthread_cond_signal(&m_shm_ptr->cv_can_write);
  pthread_mutex_unlock(&m_shm_ptr->mtx);
  return true;

#pragma clang diagnostic pop
#pragma GCC diagnostic pop
}

void ShmVideoReader::Stop() {
  m_stopped = true;
  // Wake up any reader blocked on cv_can_read
  if (m_shm_ptr) {
    pthread_mutex_lock(&m_shm_ptr->mtx);
    pthread_cond_broadcast(&m_shm_ptr->cv_can_read);
    pthread_mutex_unlock(&m_shm_ptr->mtx);
  }
}
