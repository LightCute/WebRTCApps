// shm_video_source.h
#pragma once
#include <QObject>
#include <QImage>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <vector>
#include "shm_video_reader.h"

class ShmVideoSource : public QObject
{
    Q_OBJECT

public:
    explicit ShmVideoSource(const QString& key_path, int proj_id,
                            QObject* parent = nullptr);
    ~ShmVideoSource() override;

    void start();
    void stop();
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
