// shm_video_source.h
#pragma once
#include <QObject>
#include <QImage>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <vector>
#include "shm_video_reader.h"
#include "shm_video_source_interface.h"

class ShmVideoSource : public IShmVideoSource
{
    Q_OBJECT

public:
    explicit ShmVideoSource(QObject* parent = nullptr);
    ~ShmVideoSource() override;

    bool Start(const QString& key, int proj_id) override;
    void Stop() override;
    bool isRunning() const { return running_; }

signals:
    void frameReady(QImage frame);
    void errorOccurred(const QString& message);

private:
    void readLoop();
    void tryInit();

    ShmVideoReader reader_;
    QString key_path_;
    int proj_id_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    QThread* thread_ = nullptr;
    std::vector<uint8_t> i420_buf_;
    std::vector<uint8_t> argb_buf_;
};
