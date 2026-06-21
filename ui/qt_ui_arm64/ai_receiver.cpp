#include "ai_receiver.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstdio>

AiReceiver::AiReceiver(const QString& socketPath, QObject* parent)
    : QObject(parent), socket_path_(socketPath) {}

AiReceiver::~AiReceiver() { stop(); }

void AiReceiver::start() {
    if (running_) return;
    running_ = true;
    thread_ = QThread::create([this] { run(); });
    thread_->start();
}

void AiReceiver::stop() {
    running_ = false;
    if (thread_) {
        thread_->wait(3000);
        thread_->deleteLater();
        thread_ = nullptr;
    }
}

void AiReceiver::parse(const QByteArray& json) {
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError) return;
    QJsonObject root = doc.object();
    QJsonArray dets = root["dets"].toArray();
    QVector<Detection> result;
    for (const auto& v : dets) {
        QJsonObject d = v.toObject();
        Detection det;
        det.cls_id = d["cls"].toInt();
        det.label = d["label"].toString();
        det.conf = (float)d["conf"].toDouble();
        QJsonArray box = d["box"].toArray();
        if (box.size() >= 4) {
            det.left = box[0].toInt();
            det.top = box[1].toInt();
            det.right = box[2].toInt();
            det.bottom = box[3].toInt();
        }
        result.append(det);
    }
    emit detectionsReady(result);  // always emit, empty list clears old boxes
}

void AiReceiver::run() {
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("AiReceiver socket"); return; }

    unlink(socket_path_.toStdString().c_str());
    struct sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path_.toStdString().c_str(), sizeof(addr.sun_path) - 1);
    if (::bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("AiReceiver bind");
        close(server_fd);
        return;
    }
    if (listen(server_fd, 2) < 0) {
        perror("AiReceiver listen");
        close(server_fd);
        return;
    }
    fprintf(stderr, "AiReceiver: listening on %s\n", socket_path_.toStdString().c_str());

    int client_fd = -1;
    char buf[8192];
    QByteArray line_buf;

    while (running_) {
        if (client_fd < 0) {
            client_fd = accept(server_fd, nullptr, nullptr);
            if (client_fd < 0) {
                usleep(200000);
                continue;
            }
            fprintf(stderr, "AiReceiver: client connected\n");
        }

        ssize_t n = read(client_fd, buf, sizeof(buf) - 1);
        if (n <= 0) {
            fprintf(stderr, "AiReceiver: client disconnected, clearing boxes\n");
            close(client_fd);
            client_fd = -1;
            emit detectionsReady({});
            continue;
        }
        buf[n] = '\0';
        line_buf.append(buf, n);
        while (true) {
            int idx = line_buf.indexOf('\n');
            if (idx < 0) break;
            QByteArray line = line_buf.left(idx);
            line_buf.remove(0, idx + 1);
            if (!line.isEmpty()) {
                fprintf(stderr, "AiReceiver: rx %s\n", line.constData());
                parse(line);
            }
        }
    }

    if (client_fd >= 0) close(client_fd);
    close(server_fd);
    unlink(socket_path_.toStdString().c_str());
}
