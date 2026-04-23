#include "gst_shm_transport.h"
#include <unistd.h>
#include <fcntl.h>
#include "rtc_base/logging.h"

// ==================== GstShmSender 实现 ====================
GstShmSender::GstShmSender(const std::string& shm_socket)
    : shm_socket_(shm_socket) {}

GstShmSender::~GstShmSender() {
    Destroy();
}

void GstShmSender::CleanupShmSocket() {
    if (access(shm_socket_.c_str(), F_OK) == 0) {
        unlink(shm_socket_.c_str());
        RTC_LOG(LS_INFO) << "清理共享内存文件: " << shm_socket_;
    }
}

bool GstShmSender::Init() {
    if (initialized_) return true;

    // 清理残留文件
    CleanupShmSocket();

    // 创建GST元素
    pipeline_ = gst_pipeline_new("shm-sender-pipeline");
    appsrc_ = gst_element_factory_make("appsrc", "video_src");
    GstElement* convert = gst_element_factory_make("videoconvert", "convert");
    GstElement* queue = gst_element_factory_make("queue", "queue");
    GstElement* shmsink = gst_element_factory_make("shmsink", "shm_sink");

    if (!pipeline_ || !appsrc_ || !convert || !queue || !shmsink) {
        RTC_LOG(LS_ERROR) << "GST 元素创建失败";
        return false;
    }

    // 配置appsrc (输入I420)
    GstCaps* caps = gst_caps_new_simple(
        "video/x-raw",
        "format", G_TYPE_STRING, "I420",
        "width", G_TYPE_INT, VIDEO_WIDTH,
        "height", G_TYPE_INT, VIDEO_HEIGHT,
        "framerate", GST_TYPE_FRACTION, VIDEO_FPS, 1,
        nullptr);
    g_object_set(G_OBJECT(appsrc_),
        "caps", caps,
        "format", GST_FORMAT_TIME,
        "is-live", TRUE,
        nullptr);
    gst_caps_unref(caps);

    // 配置shmsink
    g_object_set(G_OBJECT(shmsink),
        "socket-path", shm_socket_.c_str(),
        "wait-for-connection", FALSE,
        "shm-size", VIDEO_WIDTH * VIDEO_HEIGHT * 3 / 2 * 2,
        nullptr);

    // 组装管道
    gst_bin_add_many(GST_BIN(pipeline_), appsrc_, convert, queue, shmsink, nullptr);
    if (!gst_element_link_many(appsrc_, convert, queue, shmsink, nullptr)) {
        RTC_LOG(LS_ERROR) << "GST 管道链接失败";
        Destroy();
        return false;
    }

    // 启动管道
    gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    initialized_ = true;
    RTC_LOG(LS_INFO) << "GST 发送端初始化成功: " << shm_socket_;
    return true;
}
void GstShmSender::PushFrame(uint8_t* i420_data, int width, int height) {
    if (!initialized_ || !i420_data) return;

    gsize buf_size = width * height * 3 / 2;
    // ==================== 修复点1：使用堆内存分配，避免栈内存野指针 ====================
    uint8_t* frame_data = new uint8_t[buf_size];
    memcpy(frame_data, i420_data, buf_size);

    // ==================== 修复点2：正确使用 gst_buffer_new_wrapped_full ====================
    // 释放函数：delete[] 堆内存（匹配GDestroyNotify类型）
    GstBuffer* buf = gst_buffer_new_wrapped_full(
        GST_MEMORY_FLAG_READONLY,
        frame_data,          // 堆内存数据
        buf_size,
        0,
        buf_size,
        nullptr,
        [](gpointer data) {  // 正确的释放回调：lambda表达式匹配GDestroyNotify
            delete[] static_cast<uint8_t*>(data);
        }
    );

    // 设置时间戳（不变）
    GST_BUFFER_PTS(buf) = frame_count_ * (GST_SECOND / VIDEO_FPS);
    GST_BUFFER_DURATION(buf) = GST_SECOND / VIDEO_FPS;
    frame_count_++;

    // 推送数据
    gst_app_src_push_buffer(GST_APP_SRC(appsrc_), buf);
}

void GstShmSender::Destroy() {
    if (!initialized_) return;

    gst_element_set_state(pipeline_, GST_STATE_NULL);
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;
    appsrc_ = nullptr;
    initialized_ = false;
    CleanupShmSocket();
}

// ==================== GstShmReceiver 实现 ====================
GstShmReceiver::GstShmReceiver(const std::string& shm_socket)
    : shm_socket_(shm_socket) {}

GstShmReceiver::~GstShmReceiver() {
    Destroy();
}

void GstShmReceiver::CleanupShmSocket() {
    // 接收端无需删除socket
}

bool GstShmReceiver::Init() {
    // Qt端管道：shmsrc ! videoconvert ! qmlglsink (Qt渲染)
    pipeline_ = gst_pipeline_new("shm-receiver-pipeline");
    GstElement* shmsrc = gst_element_factory_make("shmsrc", "shm_src");
    GstElement* convert = gst_element_factory_make("videoconvert", "convert");
    GstElement* qtsink = gst_element_factory_make("qmlglsink", "qt_sink");

    if (!pipeline_ || !shmsrc || !convert || !qtsink) return false;

    g_object_set(G_OBJECT(shmsrc), "socket-path", shm_socket_.c_str(), nullptr);
    gst_bin_add_many(GST_BIN(pipeline_), shmsrc, convert, qtsink, nullptr);
    gst_element_link_many(shmsrc, convert, qtsink, nullptr);

    gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    return true;
}

void GstShmReceiver::Destroy() {
    if (pipeline_) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }
}