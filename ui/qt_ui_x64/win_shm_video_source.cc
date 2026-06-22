// win_shm_video_source.cc
#include "win_shm_video_source.h"
#include <libyuv/convert.h>
#include <QDebug>

WinShmVideoSource::WinShmVideoSource(QObject* parent)
    : IShmVideoSource(parent) {
    i420_buf_.resize(kFrameMaxSize);
}

WinShmVideoSource::~WinShmVideoSource() { Stop(); }

bool WinShmVideoSource::Start(const QString& key, int /*proj_id*/) {
    if (running_) return false;
    name_ = key;
    stop_requested_ = false;
    // Try to open after a short delay (same retry pattern as Linux)
    QTimer::singleShot(500, this, &WinShmVideoSource::tryOpen);
    return true;
}

void WinShmVideoSource::Stop() {
    stop_requested_ = true;
    running_ = false;
    if (thread_) {
        thread_->wait(5000);
    }
    if (mapped_) {
        UnmapViewOfFile(mapped_);
        mapped_ = nullptr;
    }
    if (hMap_) {
        CloseHandle(hMap_);
        hMap_ = nullptr;
    }
}

void WinShmVideoSource::tryOpen() {
    if (stop_requested_) return;
    std::string narrow = name_.toStdString();
    hMap_ = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, narrow.c_str());
    if (!hMap_) {
        QTimer::singleShot(500, this, &WinShmVideoSource::tryOpen);
        return;
    }
    mapped_ = MapViewOfFile(hMap_, FILE_MAP_ALL_ACCESS, 0, 0, kFrameMaxSize);
    if (!mapped_) {
        CloseHandle(hMap_); hMap_ = nullptr;
        QTimer::singleShot(500, this, &WinShmVideoSource::tryOpen);
        return;
    }

    qDebug() << "WinShmVideoSource: mapped" << name_;
    running_ = true;
    thread_ = QThread::create([this] { readLoop(); });
    connect(thread_, &QThread::finished, thread_, &QObject::deleteLater);
    thread_->start();
}

void WinShmVideoSource::readLoop() {
    while (running_) {
        // Windows ring-buffer: poll with Sleep.
        // A production implementation would use named events for signaling.
        WinVideoFrameHead* head = static_cast<WinVideoFrameHead*>(mapped_);
        if (head->data_length == 0 || head->data_length > kFrameMaxSize - sizeof(WinVideoFrameHead)) {
            Sleep(5);  // 5ms poll
            continue;
        }

        int width = head->width;
        int height = head->height;
        int y_size = width * height;
        int uv_size = y_size / 4;
        int argb_size = width * height * 4;

        if (argb_buf_.size() != static_cast<size_t>(argb_size))
            argb_buf_.resize(argb_size);

        const uint8_t* y = static_cast<const uint8_t*>(mapped_) + sizeof(WinVideoFrameHead);
        const uint8_t* u = y + y_size;
        const uint8_t* v = u + uv_size;

        libyuv::I420ToARGB(y, width, u, width / 2, v, width / 2,
                           argb_buf_.data(), width * 4, width, height);

        QImage image(argb_buf_.data(), width, height, QImage::Format_ARGB32);
        emit frameReady(image.copy());

        head->data_length = 0;  // mark as consumed
    }
    running_ = false;
}
