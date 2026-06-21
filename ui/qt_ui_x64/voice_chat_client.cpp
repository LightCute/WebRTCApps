// voice_chat_client.cpp
#include "voice_chat_client.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>

VoiceChatClient::VoiceChatClient(const QString& socket_path, QObject* parent)
    : QObject(parent), socket_path_(socket_path)
{
    connect(&sock_, &QLocalSocket::connected, this, &VoiceChatClient::connected);
    connect(&sock_, &QLocalSocket::disconnected, this, &VoiceChatClient::disconnected);
    connect(&sock_, &QLocalSocket::readyRead, this, &VoiceChatClient::onReadyRead);
    connect(&sock_, QOverload<QLocalSocket::LocalSocketError>::of(&QLocalSocket::errorOccurred),
            this, [this](QLocalSocket::LocalSocketError) {
        emit errorOccurred(sock_.errorString());
    });
}

VoiceChatClient::~VoiceChatClient()
{
    disconnectFromServer();
}

void VoiceChatClient::connectToServer()
{
    sock_.connectToServer(socket_path_);
}

void VoiceChatClient::disconnectFromServer()
{
    if (sock_.state() == QLocalSocket::ConnectedState)
        sock_.disconnectFromServer();
}

bool VoiceChatClient::isConnected() const
{
    return sock_.state() == QLocalSocket::ConnectedState;
}

void VoiceChatClient::sendJson(const QJsonObject& obj)
{
    if (!isConnected()) return;
    QByteArray data = QJsonDocument(obj).toJson(QJsonDocument::Compact) + "\n";
    sock_.write(data);
}

void VoiceChatClient::cmdStart()
{
    sendJson({{"cmd", "start"}});
}

void VoiceChatClient::cmdStop()
{
    sendJson({{"cmd", "stop"}});
}

void VoiceChatClient::onReadyRead()
{
    read_buf_.append(sock_.readAll());
    while (true) {
        int idx = read_buf_.indexOf('\n');
        if (idx < 0) break;
        QByteArray line = read_buf_.left(idx);
        read_buf_.remove(0, idx + 1);
        if (line.isEmpty()) continue;

        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(line, &err);
        if (err.error != QJsonParseError::NoError) continue;

        QJsonObject root = doc.object();
        handleEvent(root);
    }
}

void VoiceChatClient::handleEvent(const QJsonObject& event)
{
    QString evt = event["event"].toString();
    if (evt == "asr_result")
        emit asrResult(event["text"].toString());
    else if (evt == "llm_result")
        emit llmResult(event["text"].toString());
    else if (evt == "started")
        emit started();
    else if (evt == "stopped")
        emit stopped();
    else if (evt == "ready")
        emit ready();
    else if (evt == "error")
        emit errorOccurred(event["message"].toString());
}
