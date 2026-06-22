#include "dma_buf_video_source.h"
#include "dma_buf_reader.h"
#include <dlfcn.h>
#include <libyuv/convert.h>
#include <QDebug>
#include <cstring>

extern "C" {
#include "rga.h"
#include "drmrga.h"
}

DmaBufVideoSource::DmaBufVideoSource(QObject* parent)
    : IShmVideoSource(parent) {}

DmaBufVideoSource::~DmaBufVideoSource() {
    Stop();
    if (rga_lib_) dlclose(rga_lib_);
}

bool DmaBufVideoSource::Start(const QString& key, int proj_id) {
    if (running_) return false;
    ctrl_shm_path_ = key;
    proj_id_ = proj_id;
    stop_requested_ = false;
    tryInit();
    return true;
}

void DmaBufVideoSource::Stop() {
    stop_requested_ = true;
    if (!running_) return;
    running_ = false;
    if (reader_) reader_->RequestStop();
    if (thread_) thread_->wait(5000);
}

void DmaBufVideoSource::tryInit() {
    if (stop_requested_) return;

    rga_lib_ = dlopen("librga.so", RTLD_NOW | RTLD_GLOBAL);
    if (rga_lib_) {
        rga_blit_ = reinterpret_cast<int(*)(void*,void*,void*)>(
            dlsym(rga_lib_, "c_RkRgaBlit"));
        if (rga_blit_) rga_loaded_ = true;
    }

    reader_ = std::make_unique<DmaBufReader>();
    if (reader_->Init(ctrl_shm_path_.toStdString(), proj_id_,
                      socket_path_.toStdString(), CONSUMER_FLAG_SKIP_ALLOWED) == 0) {
        qDebug() << "DmaBufVideoSource: DMA-BUF reader initialized for" << ctrl_shm_path_;
        running_ = true;
        thread_ = QThread::create([this] { readLoop(); });
        connect(thread_, &QThread::finished, thread_, &QObject::deleteLater);
        connect(thread_, &QThread::finished, this, [this] { thread_ = nullptr; });
        thread_->start();
        return;
    }

    reader_.reset();
    QTimer::singleShot(500, this, &DmaBufVideoSource::tryInit);
}

void DmaBufVideoSource::readLoop() {
    while (running_) {
        VideoFrameHead head;
        const uint8_t* i420_ptr = nullptr;

        if (!reader_->ReadFrame(head, i420_ptr)) {
            continue;
        }

        int w = head.width, h = head.height;
        if (w <= 0 || h <= 0) continue;

        size_t bgra_size = w * h * 4;
        if (bgra_buf_.size() < bgra_size) bgra_buf_.resize(bgra_size);

        if (rga_loaded_) {
            rga_info_t src;
            memset(&src, 0, sizeof(src));
            src.virAddr = const_cast<uint8_t*>(i420_ptr);
            src.format = RK_FORMAT_YCbCr_420_P;
            src.rect.xoffset = 0; src.rect.yoffset = 0;
            src.rect.width = w; src.rect.height = h;
            src.rect.wstride = w; src.rect.hstride = h;
            src.rect.format = RK_FORMAT_YCbCr_420_P;
            src.mmuFlag = 1; src.sync_mode = 0;

            rga_info_t dst;
            memset(&dst, 0, sizeof(dst));
            dst.virAddr = bgra_buf_.data();
            dst.format = RK_FORMAT_BGRA_8888;
            dst.rect.xoffset = 0; dst.rect.yoffset = 0;
            dst.rect.width = w; dst.rect.height = h;
            dst.rect.wstride = w; dst.rect.hstride = h;
            dst.rect.format = RK_FORMAT_BGRA_8888;
            dst.mmuFlag = 1; dst.sync_mode = 0;

            if (rga_blit_(&src, &dst, nullptr) != 0)
                rga_loaded_ = false;
        }

        if (!rga_loaded_) {
            int y_size = w * h;
            int uv_size = y_size / 4;
            const uint8_t* y = i420_ptr;
            const uint8_t* u = y + y_size;
            const uint8_t* v = u + uv_size;
            libyuv::I420ToARGB(y, w, u, w / 2, v, w / 2,
                               bgra_buf_.data(), w * 4, w, h);
        }

        QImage img(bgra_buf_.data(), w, h, QImage::Format_ARGB32);
        emit frameReady(img.copy());              // IShmVideoSource signal
        emit frameReady(img.copy(), w, h);        // ARM64 extended signal
    }
    running_ = false;
}
