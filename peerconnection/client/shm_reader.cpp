//shm_reader.cpp
#include "shm_reader.h"
#include <iostream>

ShmVideoReader::ShmVideoReader() = default;
ShmVideoReader::~ShmVideoReader(){
    if(m_shm_ptr){
        shmdt(m_shm_ptr);
        m_shm_ptr = nullptr;
    }
}

bool ShmVideoReader::init(){
    key_t key = ftok(SHM_KEY_PATH, SHM_PROJ_ID);
    if(key == -1){
        perror("ftok failed");
        return false;
    }

    m_shmid = shmget(key, SHM_CTRL_BLOCK_SIZE, 0666);
    if(m_shmid == -1){
        perror("shmget failed");
        return false;
    }

    m_shm_ptr = reinterpret_cast<ShmCtrlBlock*>(shmat(m_shmid, nullptr, 0));
    if(reinterpret_cast<void*>(m_shm_ptr) == reinterpret_cast<void*>(-1)){
        perror("shmat failed");
        return false;
    }
    m_inited = true;
    std::cout << "[ShmReader] Shared mem inited" << std::endl;
    return true;
}


bool ShmVideoReader::read_frame(VideoFrameHead& out_head, uint8_t* out_data, uint32_t buf_size){
    // 合法性校验
    if(!m_inited || !m_shm_ptr || !out_data) 
        return false;

    // 加锁（和写端同一把跨进程锁）
    pthread_mutex_lock(&m_shm_ptr->mtx);
    std::cout << "[ShmReader] read_frame locked" << std::endl;

    // ✅ 对称逻辑：缓冲区为空 → 阻塞等待写端写入
    while (m_shm_ptr->frame_count <= 0)
    {
        std::cout << "[ShmReader] Ring empty, wait... frame_count: " 
                  << m_shm_ptr->frame_count << std::endl;
        // 等待写端唤醒
        pthread_cond_wait(&m_shm_ptr->cv_can_read, &m_shm_ptr->mtx);
    }

    // ✅ 读取数据
    std::cout << "[ShmReader] read frame, frame_count: " 
              << m_shm_ptr->frame_count << std::endl;

    auto& item = m_shm_ptr->ring[m_shm_ptr->r_idx];
    out_head = item.head;
    // 拷贝数据（防止溢出）
    if(out_head.frame_len <= buf_size){
        std::memcpy(out_data, item.data, out_head.frame_len);
    }

    // 更新读索引
    m_shm_ptr->r_idx = (m_shm_ptr->r_idx + 1) % RING_BUFFER_CNT;
    // 帧计数-1
    m_shm_ptr->frame_count--;

    // ✅ 唤醒写端：缓冲区有空位了
    pthread_cond_signal(&m_shm_ptr->cv_can_write);
    // 解锁
    pthread_mutex_unlock(&m_shm_ptr->mtx);

    return true;
}