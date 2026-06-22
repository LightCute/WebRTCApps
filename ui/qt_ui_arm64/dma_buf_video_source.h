#pragma once
#include <QObject>
#include <QImage>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <memory>
#include <vector>
#include <string>
#include <cstdint>

#include "shm_video_source_interface.h"

class DmaBufReader;

class DmaBufVideoSource : public IShmVideoSource
{
    Q_OBJECT

public:
    // IShmVideoSource API
    explicit DmaBufVideoSource(QObject* parent = nullptr);
    ~DmaBufVideoSource() override;
    bool Start(const QString& key, int proj_id) override;
    void Stop() override;

    // ARM64-specific: set DMA-BUF socket path before Start()
    void setSocketPath(const QString& path) { socket_path_ = path; }
    bool isRunning() const { return running_; }

signals:
    void frameReady(QImage frame);                    // IShmVideoSource
    void frameReady(QImage frame, int w, int h);      // ARM64 extended
    void errorOccurred(const QString& message);

private:
    void readLoop();
    void tryInit();

    std::unique_ptr<DmaBufReader> reader_;
    QString ctrl_shm_path_;
    QString socket_path_;
    int proj_id_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    QThread* thread_ = nullptr;

    // RGA I420→BGRA hardware conversion
    bool rga_loaded_ = false;
    void* rga_lib_ = nullptr;
    int (*rga_blit_)(void*, void*, void*) = nullptr;
    std::vector<uint8_t> bgra_buf_;
};
