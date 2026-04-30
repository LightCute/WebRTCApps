// shm_audio_writer.h
#pragma once
#include "shm_common.h"
#include <string>

class ShmAudioWriter
{
public:
    ShmAudioWriter();
    ~ShmAudioWriter();

    ShmAudioWriter(const ShmAudioWriter&) = delete;
    ShmAudioWriter& operator=(const ShmAudioWriter&) = delete;

    bool Init(const std::string& key_path, int proj_id);
    bool WriteFrame(const AudioFrameHead& head, const uint8_t* data);

private:
    int32_t m_shmid{-1};
    ShmAudioCtrlBlock* m_shm_ptr{nullptr};
    bool m_inited{false};
};
