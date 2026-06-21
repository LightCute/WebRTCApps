// voice_chat_client.h
#pragma once
#include <QObject>
#include <QLocalSocket>
#include <QJsonObject>

class VoiceChatClient : public QObject
{
    Q_OBJECT

public:
    explicit VoiceChatClient(const QString& socket_path, QObject* parent = nullptr);
    ~VoiceChatClient() override;

    void connectToServer();
    void disconnectFromServer();
    bool isConnected() const;
    void cmdStart();
    void cmdStop();

signals:
    void connected();
    void disconnected();
    void asrResult(const QString& text);
    void llmResult(const QString& text);
    void errorOccurred(const QString& msg);
    void started();
    void stopped();
    void ready();

private slots:
    void onReadyRead();

private:
    void sendJson(const QJsonObject& obj);
    void handleEvent(const QJsonObject& event);

    QLocalSocket sock_;
    QString socket_path_;
    QByteArray read_buf_;
};
