// shm_common.h
#pragma once
#include <cstdint>
#include <pthread.h>

#include <cstdlib>
#include <string>

inline std::string shm_key_path() {
    const char* dir = getenv("WEBRTC_RUNTIME_DIR");
    return std::string(dir ? dir : "/tmp/webrtc_runtime") + "/shm_video_buf";
}
inline constexpr int         SHM_PROJ_ID  = 0x88;

inline constexpr int FRAME_MAX_SIZE = 2 * 1024 * 1024;
inline constexpr int RING_BUFFER_CNT = 8;
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

struct RingVideoFrameItem
{
    VideoFrameHead head;
    uint8_t data[FRAME_MAX_SIZE];
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

inline constexpr size_t SHM_CTRL_BLOCK_SIZE = sizeof(ShmCtrlBlock);

int init_shm_sync(ShmCtrlBlock* shm);         // 0 on success, -1 on failure
