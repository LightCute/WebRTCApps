// shm_common.h
#pragma once
#include <cstdint>
#include <pthread.h>

inline constexpr const char* SHM_KEY_PATH = "/home/light/tmp/shm_video_buf";
inline constexpr int         SHM_PROJ_ID  = 0x88;

inline constexpr int FRAME_MAX_SIZE = 2 * 1024 * 1024;
inline constexpr int RING_BUFFER_CNT = 8;
inline constexpr int AUDIO_FRAME_MAX_SIZE = 1920 * 4;

struct VideoFrameHead
{
    int64_t ntp_time_ms;    // NTP timestamp in ms for A/V sync
    uint32_t frame_len;
    uint16_t width;
    uint16_t height;
    uint8_t frame_type;     // 0=unknown, 1=I-frame, 2=P-frame
    uint8_t rotation;
    uint8_t reserve[5];
};

struct AudioFrameHead
{
    int64_t ntp_time_ms;       // NTP timestamp in ms for A/V sync
    uint32_t frame_len;        // PCM data length in bytes
    uint32_t sample_rate;      // e.g. 48000
    uint16_t channels;         // e.g. 2
    uint16_t bits_per_sample;  // e.g. 16
};

struct RingVideoFrameItem
{
    VideoFrameHead head;
    uint8_t data[FRAME_MAX_SIZE];
};

struct RingAudioFrameItem
{
    AudioFrameHead head;
    uint8_t data[AUDIO_FRAME_MAX_SIZE];
};

struct ShmCtrlBlock
{
    pthread_mutex_t mtx;
    pthread_cond_t cv_can_write;
    pthread_cond_t cv_can_read;
    // These fields are initialized by init_shm_sync(), not by constructor
    // (SHM mmap regions do not invoke constructors).
    uint32_t w_idx;
    uint32_t r_idx;
    uint32_t frame_count;
    RingVideoFrameItem ring[RING_BUFFER_CNT];
    ShmCtrlBlock() = default;
    ~ShmCtrlBlock() = default;
};

inline constexpr int AUDIO_RING_BUFFER_CNT = 32;

struct ShmAudioCtrlBlock
{
    pthread_mutex_t mtx;
    pthread_cond_t cv_can_write;
    pthread_cond_t cv_can_read;
    // These fields are initialized by init_shm_audio_sync(), not by constructor
    // (SHM mmap regions do not invoke constructors).
    uint32_t w_idx;
    uint32_t r_idx;
    uint32_t frame_count;
    RingAudioFrameItem ring[AUDIO_RING_BUFFER_CNT];
    ShmAudioCtrlBlock() = default;
    ~ShmAudioCtrlBlock() = default;
};

inline constexpr size_t SHM_CTRL_BLOCK_SIZE = sizeof(ShmCtrlBlock);
inline constexpr size_t SHM_AUDIO_CTRL_BLOCK_SIZE = sizeof(ShmAudioCtrlBlock);

int init_shm_sync(ShmCtrlBlock* shm);         // 0 on success, -1 on failure
int init_shm_audio_sync(ShmAudioCtrlBlock* shm);
