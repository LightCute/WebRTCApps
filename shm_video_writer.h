// shm_video_writer.h
#pragma once
#include "shm_common.h"
#include <string>

class ShmVideoWriter
{
public:
    ShmVideoWriter();
    ~ShmVideoWriter();

    ShmVideoWriter(const ShmVideoWriter&) = delete;
    ShmVideoWriter& operator=(const ShmVideoWriter&) = delete;

    bool Init(const std::string& key_path, int proj_id);
    bool WriteFrame(const VideoFrameHead& head, const uint8_t* data);

private:
    int32_t m_shmid{-1};
    ShmCtrlBlock* m_shm_ptr{nullptr};
    bool m_inited{false};
};
