// control_channel.h
#pragma once
#include <QObject>
#include <QLocalSocket>
#include <QJsonObject>
#include <QJsonArray>
#include <QVector>

struct PeerInfo {
    int id;
    QString name;
};

class ControlChannel : public QObject
{
    Q_OBJECT

public:
    explicit ControlChannel(const QString& socket_path, QObject* parent = nullptr);
    ~ControlChannel() override;

    void connectToServer();
    void disconnectFromServer();
    bool isConnected() const;

    // Commands — each returns the request id for response matching
    int cmdConnect(const QString& server, int port);
    int cmdDisconnect();
    int cmdCall(int peer_id);
    int cmdHangup();
    int cmdShutdown();
    int cmdSetMute(bool audio_mute, bool video_mute);
    int cmdSendData(const QString& text);
    int cmdQueryDevices();
    int cmdSetVideoDevice(int device_idx);
    int cmdSetAudioInputDevice(int device_idx);
    void cmdStartStats();
    void cmdStopStats();

signals:
    void connected();
    void disconnected();
    void responseReceived(int id, bool ok, const QString& error);
    void serverConnected();
    void serverDisconnected();
    void serverConnectionFailed(const QString& error);
    void peerOnline(PeerInfo peer);
    void peerOffline(int peer_id);
    void peerListReceived(QVector<PeerInfo> peers);
    void callConnected();
    void callDisconnected();
    void iceStateChanged(const QString& state);
    void dataChannelStateChanged(const QString& state);
    void dataReceived(const QString& text);
    void videoDevicesReceived(QList<QPair<int, QString>> devices);
    void audioInputDevicesReceived(QList<QPair<int, QString>> devices);
    void audioOutputDevicesReceived(QList<QPair<int, QString>> devices);
    void statsReceived(const QJsonObject& stats);
    void errorOccurred(const QString& message);

private slots:
    void onReadyRead();

private:
    void sendJson(const QJsonObject& obj);
    void handleEvent(const QJsonObject& event);

    QLocalSocket sock_;
    QString socket_path_;
    QByteArray read_buf_;
    int next_id_ = 1;
};
