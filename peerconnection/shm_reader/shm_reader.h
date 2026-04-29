// shm_reader.h — 共享内存读取端
#pragma once
#include "shm_common.h"
#include <sys/ipc.h>
#include <sys/shm.h>
#include <string>

class ShmVideoReader
{
public:
    ShmVideoReader();
    ~ShmVideoReader();

    ShmVideoReader(const ShmVideoReader&) = delete;
    ShmVideoReader& operator=(const ShmVideoReader&) = delete;

    bool init(const std::string& key_path, int proj_id);
    bool read_frame(VideoFrameHead& out_head, uint8_t* out_data, uint32_t buf_size);

private:
    int32_t      m_shmid{-1};
    ShmCtrlBlock* m_shm_ptr{nullptr};
    bool         m_inited{false};
    uint32_t     m_last_r_idx{0};
};
