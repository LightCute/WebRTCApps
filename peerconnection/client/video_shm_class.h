#ifndef VIDEO_SHM_CLASS_H
#define VIDEO_SHM_CLASS_H

#include <cstdint>
#include <cstdlib>
#include <string>
#include <semaphore.h>
#include <sys/ipc.h>
#include <sys/shm.h>

// ===================== 固定视频配置 =====================
#define VIDEO_WIDTH       640
#define VIDEO_HEIGHT      480
#define I420_FRAME_SIZE   (VIDEO_WIDTH * VIDEO_HEIGHT * 3 / 2)

// 共享内存类型（本地/远端）
enum ShmType {
    LOCAL_SHM,
    REMOTE_SHM
};

// 头部结构体
struct VideoShmHeader {
    bool ready;
    int width;
    int height;
    size_t data_len;
};

const size_t SHM_TOTAL_SIZE = sizeof(VideoShmHeader) + I420_FRAME_SIZE;

// 发送端
class ShmSender {
public:
    explicit ShmSender(ShmType type);
    ~ShmSender();
    void PushFrame(const uint8_t* i420_data, int width, int height);

    ShmSender(const ShmSender&) = delete;
    ShmSender& operator=(const ShmSender&) = delete;

private:
    bool Init();
    void Release();

    ShmType type_;
    int shmid_;
    char* shm_addr_;
    VideoShmHeader* header_;
    uint8_t* video_data_;
    sem_t* sem_;
    bool inited_;
};

// 接收端
class ShmReceiver {
public:
    explicit ShmReceiver(ShmType type);
    ~ShmReceiver();
    bool ReadFrame(uint8_t* out_i420_data, int& out_width, int& out_height);

    ShmReceiver(const ShmReceiver&) = delete;
    ShmReceiver& operator=(const ShmReceiver&) = delete;

private:
    bool Init();
    void Release();

    ShmType type_;
    int shmid_;
    char* shm_addr_;
    VideoShmHeader* header_;
    uint8_t* video_data_;
    sem_t* sem_;
    bool inited_;
};

#endif // VIDEO_SHM_CLASS_H