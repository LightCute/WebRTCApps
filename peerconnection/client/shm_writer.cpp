//shm_writer.cpp
#include "shm_writer.h" 
#include <iostream>
#include <unistd.h>

ShmVideoWriter::ShmVideoWriter() = default;

ShmVideoWriter::~ShmVideoWriter(){
    if(m_shm_ptr){
        shmdt(m_shm_ptr);
        m_shm_ptr = nullptr;
    }
    if(m_shmid >= 0){
        shmctl(m_shmid, IPC_RMID, nullptr);
        m_shmid = -1;
    }
}

bool ShmVideoWriter::init(){
    key_t key = ftok(SHM_KEY_PATH, SHM_PROJ_ID);
    if(key == -1){
        perror("ftok failed");
        return false;
    }

    m_shmid = shmget(key, SHM_CTRL_BLOCK_SIZE, IPC_CREAT | 0666);
    if(m_shmid == -1){
        perror("shmget failed");
        return false;
    }

    m_shm_ptr = reinterpret_cast<ShmCtrlBlock*>(shmat(m_shmid, nullptr, 0));
    if(reinterpret_cast<void*>(m_shm_ptr) == reinterpret_cast<void*>(-1)){
        perror("shmat  failed");
        return false;
    }

    new (m_shm_ptr) ShmCtrlBlock{};

    init_shm_sync(m_shm_ptr);
    m_inited = true;
    std::cout << "[ShmWriter] shared mem inited" << std::endl;
    return true;
}

bool ShmVideoWriter::write_frame(const VideoFrameHead& head, const uint8_t* data){
    if(!m_inited || !m_shm_ptr || !data) 
        return false;
    pthread_mutex_lock(&m_shm_ptr->mtx);
    std::cout << "[ShmWriter] write_frame locked" << std::endl;


    // ✅ 正确逻辑：缓冲区满了 → 等待消费者唤醒
    while (m_shm_ptr->frame_count >= RING_BUFFER_CNT)
    {
        std::cout << "[ShmWriter] Ring full, wait... frame_count: " 
                  << m_shm_ptr->frame_count << std::endl;
        // 等待有空位
        pthread_cond_wait(&m_shm_ptr->cv_can_write, &m_shm_ptr->mtx);
    }

    // ✅ 缓冲区未满 → 写入数据
    std::cout << "[ShmWriter] write frame, frame_count: " 
              << m_shm_ptr->frame_count << std::endl;
              
    auto& item = m_shm_ptr->ring[m_shm_ptr->w_idx];
    item.head = head;
    std::memcpy(item.data, data, head.frame_len);
    m_shm_ptr->w_idx = (m_shm_ptr->w_idx + 1) % RING_BUFFER_CNT;
    m_shm_ptr->frame_count++;
    pthread_cond_signal(&m_shm_ptr->cv_can_read);
    pthread_mutex_unlock(&m_shm_ptr->mtx);
    return true;



}

