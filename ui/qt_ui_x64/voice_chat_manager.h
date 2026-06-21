// voice_chat_manager.h
#pragma once
#include <QObject>
#include <QProcess>

class VoiceChatManager : public QObject
{
    Q_OBJECT

public:
    explicit VoiceChatManager(QObject* parent = nullptr);
    ~VoiceChatManager() override;

    void start(const QString& socket_path);
    void stop();
    QProcess* process() { return &proc_; }

signals:
    void daemonLog(const QString& line);

private:
    QProcess proc_;
};
