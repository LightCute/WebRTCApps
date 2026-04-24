#include "apps/peerconnection/client/video_shm_class.h"
#include <cstring>
#include <iostream>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunsafe-buffer-usage"
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"

static key_t GetShmKey(ShmType type) {
    const char* path = (type == LOCAL_SHM) ? "/tmp/webrtc_local" : "/tmp/webrtc_remote";
    int proj_id = (type == LOCAL_SHM) ? 888 : 999;
    return ftok(path, proj_id);
}

static const char* GetSemName(ShmType type) {
    return (type == LOCAL_SHM) ? "/video_local_sem" : "/video_remote_sem";
}

// ===================== ShmSender =====================
ShmSender::ShmSender(ShmType type)
    : type_(type), shmid_(-1), shm_addr_(nullptr), header_(nullptr),
      video_data_(nullptr), sem_(nullptr), inited_(false) {
    inited_ = Init();
    if (!inited_) std::cerr << "ShmSender init failed" << std::endl;
}

bool ShmSender::Init() {
    key_t key = GetShmKey(type_);
    if (key == -1) return false;

    shmid_ = shmget(key, SHM_TOTAL_SIZE, IPC_CREAT | 0666);
    if (shmid_ == -1) return false;

    shm_addr_ = (char*)shmat(shmid_, nullptr, 0);
    if (shm_addr_ == (void*)-1) return false;

    header_ = reinterpret_cast<VideoShmHeader*>(shm_addr_);
    video_data_ = reinterpret_cast<uint8_t*>(&header_[1]);

    sem_ = sem_open(GetSemName(type_), O_CREAT, 0666, 0);
    if (sem_ == SEM_FAILED) return false;

    header_->ready = false;
    header_->width = VIDEO_WIDTH;
    header_->height = VIDEO_HEIGHT;
    header_->data_len = I420_FRAME_SIZE;
    return true;
}

void ShmSender::PushFrame(const uint8_t* i420_data, int width, int height) {
    if (!inited_ || !i420_data) return;
    memcpy(video_data_, i420_data, I420_FRAME_SIZE);
    header_->width = width;
    header_->height = height;
    header_->ready = true;
    sem_post(sem_);
}

void ShmSender::Release() {
    if (shm_addr_ != (void*)-1 && shm_addr_) shmdt(shm_addr_);
    if (sem_) sem_close(sem_);
}

ShmSender::~ShmSender() { Release(); }

// ===================== ShmReceiver =====================
ShmReceiver::ShmReceiver(ShmType type)
    : type_(type), shmid_(-1), shm_addr_(nullptr), header_(nullptr),
      video_data_(nullptr), sem_(nullptr), inited_(false) {
    inited_ = Init();
}

bool ShmReceiver::Init() {
    key_t key = GetShmKey(type_);
    if (key == -1) return false;

    shmid_ = shmget(key, SHM_TOTAL_SIZE, 0666);
    if (shmid_ == -1) return false;

    shm_addr_ = (char*)shmat(shmid_, nullptr, 0);
    if (shm_addr_ == (void*)-1) return false;

    header_ = reinterpret_cast<VideoShmHeader*>(shm_addr_);
    video_data_ = reinterpret_cast<uint8_t*>(&header_[1]);

    sem_ = sem_open(GetSemName(type_), 0);
    if (sem_ == SEM_FAILED) return false;
    return true;
}

bool ShmReceiver::ReadFrame(uint8_t* out_i420_data, int& out_width, int& out_height) {
    if (!inited_ || !out_i420_data) return false;
    sem_wait(sem_);
    if (!header_->ready) return false;

    memcpy(out_i420_data, video_data_, I420_FRAME_SIZE);
    out_width = header_->width;
    out_height = header_->height;
    header_->ready = false;
    return true;
}

void ShmReceiver::Release() {
    if (shm_addr_ != (void*)-1 && shm_addr_) shmdt(shm_addr_);
    shmctl(shmid_, IPC_RMID, nullptr);
    if (sem_) {
        sem_close(sem_);
        sem_unlink(GetSemName(type_));
    }
}

ShmReceiver::~ShmReceiver() { Release(); }

#pragma GCC diagnostic pop