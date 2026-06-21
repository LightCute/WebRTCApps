// webrtc_process_manager.h
#pragma once
#include <QObject>
#include <QProcess>
#include <QTimer>

class WebRtcProcessManager : public QObject
{
    Q_OBJECT

public:
    enum State { Stopped, Starting, Running, Error };

    explicit WebRtcProcessManager(const QString& binary_path, QObject* parent = nullptr);
    ~WebRtcProcessManager() override;

    void start();
    void stop();
    State state() const { return state_; }
    QProcess* process() { return &proc_; }

signals:
    void stateChanged(WebRtcProcessManager::State newState);
    void daemonLog(const QString& line);

private slots:
    void onProcessStarted();
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onProcessError(QProcess::ProcessError error);
    void onReadyStdout();
    void onReadyStderr();

private:
    void setState(State s);
    void retryStart();

    QProcess proc_;
    QString binary_path_;
    State state_ = Stopped;
    int retry_count_ = 0;
    static constexpr int kMaxRetries = 2;
};
