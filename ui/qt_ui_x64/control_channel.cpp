// control_channel.cpp
#include "control_channel.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>

ControlChannel::ControlChannel(const QString& socket_path, QObject* parent)
    : QObject(parent), socket_path_(socket_path)
{
    connect(&sock_, &QLocalSocket::connected, this, &ControlChannel::connected);
    connect(&sock_, &QLocalSocket::disconnected, this, &ControlChannel::disconnected);
    connect(&sock_, &QLocalSocket::readyRead, this, &ControlChannel::onReadyRead);
    connect(&sock_, QOverload<QLocalSocket::LocalSocketError>::of(&QLocalSocket::errorOccurred),
            this, [this](QLocalSocket::LocalSocketError) {
        emit errorOccurred(sock_.errorString());
    });
}

ControlChannel::~ControlChannel() {
    disconnectFromServer();
}

void ControlChannel::connectToServer() {
    sock_.connectToServer(socket_path_);
}

void ControlChannel::disconnectFromServer() {
    if (sock_.state() == QLocalSocket::ConnectedState) {
        sock_.disconnectFromServer();
    }
}

bool ControlChannel::isConnected() const {
    return sock_.state() == QLocalSocket::ConnectedState;
}

void ControlChannel::sendJson(const QJsonObject& obj) {
    if (!isConnected()) return;
    QByteArray data = QJsonDocument(obj).toJson(QJsonDocument::Compact) + "\n";
    sock_.write(data);
}

int ControlChannel::cmdConnect(const QString& server, int port) {
    int id = next_id_++;
    QJsonObject obj;
    obj["id"] = id;
    obj["cmd"] = "connect";
    QJsonObject params;
    params["server"] = server;
    params["port"] = port;
    obj["params"] = params;
    sendJson(obj);
    return id;
}

int ControlChannel::cmdDisconnect() {
    int id = next_id_++;
    sendJson({{"id", id}, {"cmd", "disconnect"}});
    return id;
}

int ControlChannel::cmdCall(int peer_id) {
    int id = next_id_++;
    QJsonObject obj;
    obj["id"] = id;
    obj["cmd"] = "call";
    obj["params"] = QJsonObject{{"peer_id", peer_id}};
    sendJson(obj);
    return id;
}

int ControlChannel::cmdHangup() {
    int id = next_id_++;
    sendJson({{"id", id}, {"cmd", "hangup"}});
    return id;
}

int ControlChannel::cmdShutdown() {
    int id = next_id_++;
    sendJson({{"id", id}, {"cmd", "shutdown"}});
    return id;
}

int ControlChannel::cmdQueryDevices() {
    int id = next_id_++;
    sendJson({{"id", id}, {"cmd", "query_devices"}});
    return id;
}

int ControlChannel::cmdSetVideoDevice(int device_idx) {
    int id = next_id_++;
    QJsonObject obj;
    obj["id"] = id;
    obj["cmd"] = "set_video_device";
    obj["params"] = QJsonObject{{"device_idx", device_idx}};
    sendJson(obj);
    return id;
}

int ControlChannel::cmdSetAudioInputDevice(int device_idx) {
    int id = next_id_++;
    QJsonObject obj;
    obj["id"] = id;
    obj["cmd"] = "set_audio_input_device";
    obj["params"] = QJsonObject{{"device_idx", device_idx}};
    sendJson(obj);
    return id;
}

int ControlChannel::cmdSetMute(bool audio_mute, bool video_mute) {
    int id = next_id_++;
    QJsonObject obj;
    obj["id"] = id;
    obj["cmd"] = "set_mute";
    obj["params"] = QJsonObject{{"audio", audio_mute}, {"video", video_mute}};
    sendJson(obj);
    return id;
}

int ControlChannel::cmdSendData(const QString& text) {
    int id = next_id_++;
    QJsonObject obj;
    obj["id"] = id;
    obj["cmd"] = "send_data";
    obj["params"] = QJsonObject{{"text", text}};
    sendJson(obj);
    return id;
}


void ControlChannel::cmdStartStats() {
    QJsonObject obj;
    obj["id"] = next_id_++;
    obj["cmd"] = "start_stats";
    sendJson(obj);
}

void ControlChannel::cmdStopStats() {
    QJsonObject obj;
    obj["id"] = next_id_++;
    obj["cmd"] = "stop_stats";
    sendJson(obj);
}
void ControlChannel::onReadyRead() {
    read_buf_.append(sock_.readAll());
    if (read_buf_.size() > 1 << 20) {
        qWarning() << "ControlChannel: read buffer overflow, discarding data";
        read_buf_.clear();
    }
    while (true) {
        int idx = read_buf_.indexOf('\n');
        if (idx < 0) break;
        QByteArray line = read_buf_.left(idx);
        read_buf_.remove(0, idx + 1);
        if (line.isEmpty()) continue;

        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(line, &err);
        if (err.error != QJsonParseError::NoError) {
            qWarning() << "JSON parse error:" << err.errorString();
            continue;
        }
        QJsonObject root = doc.object();

        if (root.contains("id") && !root.contains("event")) {
            // Response
            int id = root["id"].toInt();
            bool ok = root["ok"].toBool();
            QString error = root["error"].toString();
            emit responseReceived(id, ok, error);
        } else if (root.contains("event")) {
            // Event
            handleEvent(root);
        } else {
            qWarning() << "ControlChannel: message with no id or event field dropped";
        }
    }
}

void ControlChannel::handleEvent(const QJsonObject& event) {
    QString evt = event["event"].toString();

    if (evt == "server_connected") {
        emit serverConnected();
    } else if (evt == "server_disconnected") {
        emit serverDisconnected();
    } else if (evt == "server_connection_failed") {
        emit serverConnectionFailed(event.contains("error") ? event["error"].toString() : QString());
    } else if (evt == "peer_online") {
        if (!event.contains("peer") || !event["peer"].isObject()) {
            qWarning() << "peer_online event missing peer object";
            return;
        }
        QJsonObject p = event["peer"].toObject();
        emit peerOnline({p["id"].toInt(), p["name"].toString()});
    } else if (evt == "peer_offline") {
        emit peerOffline(event["peer_id"].toInt());
    } else if (evt == "peer_list") {
        if (!event.contains("peers") || !event["peers"].isArray()) {
            qWarning() << "peer_list event missing peers array";
            return;
        }
        QVector<PeerInfo> peers;
        for (const auto& v : event["peers"].toArray()) {
            QJsonObject p = v.toObject();
            peers.append({p["id"].toInt(), p["name"].toString()});
        }
        emit peerListReceived(peers);
    } else if (evt == "call_connected") {
        emit callConnected();
    } else if (evt == "call_disconnected") {
        emit callDisconnected();
    } else if (evt == "ice_state") {
        if (event.contains("state"))
            emit iceStateChanged(event["state"].toString());
    } else if (evt == "data_channel_state") {
        if (event.contains("state"))
            emit dataChannelStateChanged(event["state"].toString());
    } else if (evt == "data_received") {
        emit dataReceived(event["text"].toString());
    } else if (evt == "video_devices") {
        QList<QPair<int, QString>> devices;
        for (const auto& v : event["devices"].toArray()) {
            QJsonObject d = v.toObject();
            devices.append({d["idx"].toInt(), d["name"].toString()});
        }
        emit videoDevicesReceived(devices);
    } else if (evt == "audio_input_devices") {
        QList<QPair<int, QString>> devices;
        for (const auto& v : event["devices"].toArray()) {
            QJsonObject d = v.toObject();
            devices.append({d["idx"].toInt(), d["name"].toString()});
        }
        emit audioInputDevicesReceived(devices);
    } else if (evt == "audio_output_devices") {
        QList<QPair<int, QString>> devices;
        for (const auto& v : event["devices"].toArray()) {
            QJsonObject d = v.toObject();
            devices.append({d["idx"].toInt(), d["name"].toString()});
        }
        emit audioOutputDevicesReceived(devices);
    } else if (evt == "stats") {
        emit statsReceived(event);
    } else {
        qDebug() << "Unknown event:" << evt;
    }
}
