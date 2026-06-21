#pragma once
#include <QObject>
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QQueue>
#include <QString>
#include <QByteArray>
#include <atomic>

class SerialWorker : public QObject
{
    Q_OBJECT

public:
    explicit SerialWorker(const QString& device, int baudRate, QObject* parent = nullptr);
    ~SerialWorker() override;

    void start();
    void stop();
    void enqueue(const QString& json);

signals:
    void received(const QString& json);
    void errorOccurred(const QString& msg);

private:
    void run();
    int readFrame();
    static uint8_t calcXor(const uint8_t* data, int len);

    QString device_;
    int baud_rate_;
    int fd_ = -1;
    std::atomic<bool> running_{false};

    QMutex tx_mutex_;
    QQueue<QByteArray> tx_queue_;
    QWaitCondition tx_cond_;

    QThread* thread_ = nullptr;

    enum RxState { WAIT_H1, WAIT_H2, WAIT_LEN, WAIT_DATA, WAIT_XOR, WAIT_CR, WAIT_LF };
    RxState rx_state_ = WAIT_H1;
    uint8_t rx_buf_[256];
    int rx_pos_ = 0;
    int rx_data_len_ = 0;
    uint8_t rx_xor_ = 0;
};
