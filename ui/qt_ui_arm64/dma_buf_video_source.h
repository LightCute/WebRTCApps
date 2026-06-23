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

class DmaBufReader;

class DmaBufVideoSource : public QObject
{
    Q_OBJECT

public:
    explicit DmaBufVideoSource(const QString& ctrl_shm_path, int proj_id,
                                const QString& socket_path,
                                QObject* parent = nullptr);
    ~DmaBufVideoSource() override;

    void start();
    void stop();
    bool isRunning() const { return running_; }

signals:
    void frameReady(QImage frame, int width, int height);
    void errorOccurred(const QString& message);

private:
    void readLoop();
    void tryInit();

    std::unique_ptr<DmaBufReader> reader_;
    QString ctrl_shm_path_;
    QString socket_path_;
    int proj_id_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    QThread* thread_ = nullptr;

    // RGA I420->BGRA hardware conversion
    bool rga_loaded_ = false;
    void* rga_lib_ = nullptr;
    int (*rga_blit_)(void*, void*, void*) = nullptr;
    std::vector<uint8_t> bgra_buf_;
};
