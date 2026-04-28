//shm_common.h
#pragma once
#include <cstdint>
#include <cstring>
#include <pthread.h>

inline constexpr const char* SHM_KEY_PATH = "/home/light/tmp/shm_video_buf";
inline constexpr int         SHM_PROJ_ID  = 0x88;

inline constexpr int FRAME_MAX_SIZE = 2 * 1024 * 1024;
inline constexpr int RING_BUFFER_CNT = 8;

struct VideoFrameHead       
{
    uint64_t timestamp;
    uint32_t frame_len;
    uint16_t width;
    uint16_t height;
    uint8_t frame_type;
    uint8_t reserve[7];
};

struct RingFrameItem    
{
    VideoFrameHead head;
    uint8_t data[FRAME_MAX_SIZE];
};

struct ShmCtrlBlock
{
    pthread_mutex_t mtx;
    pthread_cond_t cv_can_write;
    pthread_cond_t cv_can_read;
    uint32_t w_idx = 0;
    uint32_t r_idx = 0;
    uint32_t frame_count = 0;
    RingFrameItem ring[RING_BUFFER_CNT];
    ShmCtrlBlock() = default;
    ~ShmCtrlBlock() = default;
};

inline constexpr size_t SHM_CTRL_BLOCK_SIZE = sizeof(ShmCtrlBlock);
void init_shm_sync(ShmCtrlBlock* shm);

