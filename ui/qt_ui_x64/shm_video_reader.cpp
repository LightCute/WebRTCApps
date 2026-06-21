// shm_video_reader.cpp
#include "shm_video_reader.h"
#include <sys/ipc.h>
#include <sys/shm.h>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

ShmVideoReader::ShmVideoReader() = default;

ShmVideoReader::~ShmVideoReader() {
    if (m_shm_ptr) {
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

bool ShmVideoReader::ReadFrame(VideoFrameHead& out_head, uint8_t* out_data, uint32_t buf_size) {
    if (!m_inited || !out_data) return false;

    pthread_mutex_lock(&m_shm_ptr->mtx);

    while (m_shm_ptr->frame_count <= 0) {
        if (stop_requested_) {
            pthread_mutex_unlock(&m_shm_ptr->mtx);
            return false;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100 * 1000000; // 100ms
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000;
        }
        int ret = pthread_cond_timedwait(&m_shm_ptr->cv_can_read, &m_shm_ptr->mtx, &ts);
        if (ret != 0 && ret != ETIMEDOUT) {
            pthread_mutex_unlock(&m_shm_ptr->mtx);
            return false;
        }
    }

    RingVideoFrameItem& item = m_shm_ptr->ring[m_shm_ptr->r_idx];
    out_head = item.head;
    if (out_head.frame_len <= buf_size && out_head.frame_len <= FRAME_MAX_SIZE) {
        memcpy(out_data, item.data, out_head.frame_len);
        m_shm_ptr->r_idx = (m_shm_ptr->r_idx + 1) % RING_BUFFER_CNT;
        m_shm_ptr->frame_count--;

        pthread_cond_signal(&m_shm_ptr->cv_can_write);
        pthread_mutex_unlock(&m_shm_ptr->mtx);
        return true;
    } else {
        // Buffer too small - skip frame but advance ring
        m_shm_ptr->r_idx = (m_shm_ptr->r_idx + 1) % RING_BUFFER_CNT;
        m_shm_ptr->frame_count--;

        pthread_cond_signal(&m_shm_ptr->cv_can_write);
        pthread_mutex_unlock(&m_shm_ptr->mtx);
        return false;
    }
}

void ShmVideoReader::RequestStop() {
    stop_requested_ = true;
}
