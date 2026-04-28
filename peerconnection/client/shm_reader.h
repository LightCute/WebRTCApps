//shm_reader.h
#pragma once
#include "shm_common.h"
#include <sys/ipc.h>
#include <sys/shm.h>

class ShmVideoReader
{
private:
    int32_t m_shmid{-1};
    ShmCtrlBlock* m_shm_ptr{nullptr};
    bool m_inited{false};
public:
    ShmVideoReader(/* args */);
    ~ShmVideoReader();
    ShmVideoReader(const ShmVideoReader&) = delete;
    ShmVideoReader& operator=(const ShmVideoReader&) = delete;

    bool init();
    bool read_frame(VideoFrameHead& out_head, uint8_t* out_data, uint32_t buf_size);
};

