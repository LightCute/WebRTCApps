// win_shm_video_source.h — Windows SHM video source (CreateFileMapping).
//
// Identical API to ShmVideoSource — just swap the implementation
// when compiling for Windows.
#pragma once
#include <QObject>
#include <QImage>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <vector>
#include <windows.h>

#include "shm_video_source_interface.h"

struct WinVideoFrameHead {
    int64_t timestamp_us;
    uint32_t width;
    uint32_t height;
    uint32_t frame_type;
    uint32_t rotation;
    uint32_t data_length;
};

class WinShmVideoSource : public IShmVideoSource {
    Q_OBJECT

public:
    explicit WinShmVideoSource(QObject* parent = nullptr);
    ~WinShmVideoSource() override;

    bool Start(const QString& key, int proj_id) override;
    void Stop() override;

signals:
    void frameReady(QImage frame);
    void errorOccurred(const QString& message);

private:
    void readLoop();
    void tryOpen();

    HANDLE hMap_ = nullptr;
    void* mapped_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    QThread* thread_ = nullptr;
    QString name_;
    std::vector<uint8_t> i420_buf_;
    std::vector<uint8_t> argb_buf_;
    static constexpr size_t kFrameMaxSize = 2 * 1024 * 1024;
};
