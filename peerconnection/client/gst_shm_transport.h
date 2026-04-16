#ifndef GST_SHM_TRANSPORT_H_
#define GST_SHM_TRANSPORT_H_

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <string>
#include <atomic>

// 共享内存传输配置（与原工程保持一致）
#define VIDEO_WIDTH     640
#define VIDEO_HEIGHT    480
#define VIDEO_FPS       30
#define LOCAL_SHM_SOCK  "/tmp/webrtc_local_shm"
#define REMOTE_SHM_SOCK "/tmp/webrtc_remote_shm"

/**
 * @brief GStreamer 共享内存发送端（WebRTC 端使用）
 * 功能：接收 I420 格式视频帧 → 写入共享内存
 */
class GstShmSender {
public:
    explicit GstShmSender(const std::string& shm_socket);
    ~GstShmSender();

    // 初始化GST管道
    bool Init();
    // 推送I420格式视频帧
    void PushFrame(uint8_t* i420_data, int width, int height);
    // 销毁管道
    void Destroy();

private:
    // 清理残留的共享内存文件
    void CleanupShmSocket();

    std::string shm_socket_;
    GstElement* pipeline_ = nullptr;
    GstElement* appsrc_ = nullptr;
    std::atomic<bool> initialized_{false};
    guint frame_count_ = 0;
};

/**
 * @brief GStreamer 共享内存接收端（Qt 端使用）
 * 功能：从共享内存读取视频帧 → 输出给Qt渲染
 */
class GstShmReceiver {
public:
    explicit GstShmReceiver(const std::string& shm_socket);
    ~GstShmReceiver();

    bool Init();
    void Destroy();

private:
    void CleanupShmSocket();
    std::string shm_socket_;
    GstElement* pipeline_ = nullptr;
};

#endif // GST_SHM_TRANSPORT_H_