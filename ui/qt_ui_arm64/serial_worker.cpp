#include "serial_worker.h"
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

static const uint8_t FRAME_H1 = 0xAA;
static const uint8_t FRAME_H2 = 0x55;

SerialWorker::SerialWorker(const QString& device, int baudRate, QObject* parent)
    : QObject(parent), device_(device), baud_rate_(baudRate) {}

SerialWorker::~SerialWorker() { stop(); }

void SerialWorker::start() {
    if (running_) return;
    running_ = true;
    thread_ = QThread::create([this] { run(); });
    thread_->start();
}

void SerialWorker::stop() {
    running_ = false;
    tx_cond_.wakeAll();
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    if (thread_) {
        thread_->wait(3000);
        thread_->deleteLater();
        thread_ = nullptr;
    }
}

void SerialWorker::enqueue(const QString& json) {
    QMutexLocker lock(&tx_mutex_);
    tx_queue_.enqueue(json.toUtf8());
    tx_cond_.wakeOne();
}

uint8_t SerialWorker::calcXor(const uint8_t* data, int len) {
    uint8_t x = 0;
    for (int i = 0; i < len; i++) x ^= data[i];
    return x;
}

int SerialWorker::readFrame() {
    uint8_t byte;
    while (::read(fd_, &byte, 1) == 1) {
        switch (rx_state_) {
        case WAIT_H1:
            if (byte == FRAME_H1) rx_state_ = WAIT_H2;
            break;
        case WAIT_H2:
            rx_state_ = (byte == FRAME_H2) ? WAIT_LEN : WAIT_H1;
            break;
        case WAIT_LEN:
            rx_data_len_ = byte;
            rx_pos_ = 0;
            rx_xor_ = byte;
            rx_state_ = (rx_data_len_ > 0 && rx_data_len_ <= 255) ? WAIT_DATA : WAIT_H1;
            break;
        case WAIT_DATA:
            rx_buf_[rx_pos_++] = byte;
            rx_xor_ ^= byte;
            if (rx_pos_ >= rx_data_len_) rx_state_ = WAIT_XOR;
            break;
        case WAIT_XOR:
            if (byte == rx_xor_) rx_state_ = WAIT_CR;
            else { rx_state_ = WAIT_H1; }
            break;
        case WAIT_CR:
            rx_state_ = (byte == '\r') ? WAIT_LF : WAIT_H1;
            break;
        case WAIT_LF:
            if (byte == '\n') {
                rx_state_ = WAIT_H1;
                return rx_data_len_;
            }
            rx_state_ = WAIT_H1;
            break;
        }
    }
    return -1;
}

void SerialWorker::run() {
    fd_ = ::open(device_.toStdString().c_str(), O_RDWR | O_NOCTTY | O_NDELAY);
    if (fd_ < 0) {
        emit errorOccurred(QString("Failed to open %1: %2").arg(device_, strerror(errno)));
        running_ = false;
        return;
    }

    struct termios opt;
    tcgetattr(fd_, &opt);
    cfsetispeed(&opt, B115200);
    cfsetospeed(&opt, B115200);
    opt.c_cflag |= (CLOCAL | CREAD);
    opt.c_cflag &= ~PARENB;
    opt.c_cflag &= ~CSTOPB;
    opt.c_cflag &= ~CSIZE;
    opt.c_cflag |= CS8;
    opt.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    opt.c_iflag &= ~(IXON | IXOFF | IXANY);
    opt.c_oflag &= ~OPOST;
    opt.c_cc[VMIN] = 1;
    opt.c_cc[VTIME] = 0;
    tcsetattr(fd_, TCSANOW, &opt);
    ssize_t __attribute__((unused)) _f = ::write(fd_, "\r\n", 2);

    struct pollfd pfd;
    pfd.fd = fd_;
    pfd.events = POLLIN;

    while (running_) {
        // Drain TX queue first
        {
            QMutexLocker lock(&tx_mutex_);
            while (!tx_queue_.isEmpty()) {
                QByteArray payload = tx_queue_.dequeue();
                lock.unlock();
                uint8_t buf[262];
                int pos = 0;
                buf[pos++] = FRAME_H1;
                buf[pos++] = FRAME_H2;
                uint8_t len = static_cast<uint8_t>(payload.size());
                buf[pos++] = len;
                memcpy(buf + pos, payload.constData(), len);
                pos += len;
                uint8_t x = len;
                for (int i = 0; i < len; i++) x ^= payload.at(i);
                buf[pos++] = x;
                buf[pos++] = '\r';
                buf[pos++] = '\n';
                ssize_t __attribute__((unused)) w = ::write(fd_, buf, pos);
                lock.relock();
            }
        }

        // Block on poll: wake on RX data, or 50ms timeout to check TX queue
        int ret = poll(&pfd, 1, 50);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (ret > 0 && (pfd.revents & POLLIN)) {
            int data_len = readFrame();
            if (data_len > 0) {
                QByteArray json(reinterpret_cast<const char*>(rx_buf_), data_len);
                emit received(QString::fromUtf8(json));
            }
        }
    }

    if (fd_ >= 0) { close(fd_); fd_ = -1; }
}
