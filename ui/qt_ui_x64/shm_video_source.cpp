// shm_video_source.cpp
#include "shm_video_source.h"
#include <libyuv/convert.h>
#include <QDebug>

ShmVideoSource::ShmVideoSource(const QString& key_path, int proj_id,
                               QObject* parent)
    : QObject(parent), key_path_(key_path), proj_id_(proj_id)
{
    i420_buf_.resize(FRAME_MAX_SIZE);
}

ShmVideoSource::~ShmVideoSource() {
    stop();
}

void ShmVideoSource::start() {
    if (running_) return;
    stop_requested_ = false;

    // 尝试初始化。如果失败（通话还没开始，daemon 未创建文件），
    // 用定时器异步重试，不阻塞调用线程。
    tryInit();
}

void ShmVideoSource::tryInit() {
    if (stop_requested_) return;
    if (reader_.Init(key_path_.toStdString(), proj_id_)) {
        qDebug() << "ShmVideoSource: SHM reader initialized for" << key_path_;
        running_ = true;
        thread_ = QThread::create([this] { readLoop(); });
        connect(thread_, &QThread::finished, thread_, &QObject::deleteLater);
        connect(thread_, &QThread::finished, this, [this] { thread_ = nullptr; });
        thread_->start();
        return;
    }
    // 500ms 后重试
    QTimer::singleShot(500, this, &ShmVideoSource::tryInit);
}

void ShmVideoSource::stop() {
    stop_requested_ = true;  // 同时打断 Init 的重试循环
    if (!running_) return;
    running_ = false;
    reader_.RequestStop();
    if (thread_) {
        thread_->wait(5000);
    }
}

void ShmVideoSource::readLoop() {
    while (running_) {
        VideoFrameHead head;
        if (!reader_.ReadFrame(head, i420_buf_.data(), FRAME_MAX_SIZE)) {
            if (running_) {
                emit errorOccurred(QString("ReadFrame failed for %1").arg(key_path_));
            }
            break;
        }

        int width = head.width;
        int height = head.height;
        int y_size = width * height;
        int uv_size = y_size / 4;
        int argb_size = width * height * 4;

        if (argb_buf_.size() != static_cast<size_t>(argb_size)) {
            argb_buf_.resize(argb_size);
        }

        const uint8_t* y = i420_buf_.data();
        const uint8_t* u = y + y_size;
        const uint8_t* v = u + uv_size;

        int result = libyuv::I420ToARGB(
            y, width,
            u, width / 2,
            v, width / 2,
            argb_buf_.data(), width * 4,
            width, height);

        if (result != 0) {
            qWarning() << "I420ToARGB failed:" << result;
            continue;
        }

        QImage image(argb_buf_.data(), width, height, QImage::Format_ARGB32);
        QImage copy = image.copy(); // detach before next frame overwrites buffer

        emit frameReady(copy);
    }
    running_ = false;
}
