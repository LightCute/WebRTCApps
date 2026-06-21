// webrtc_process_manager.cpp
#include "webrtc_process_manager.h"
#include <QDebug>

WebRtcProcessManager::WebRtcProcessManager(const QString& binary_path,
                                           QObject* parent)
    : QObject(parent), binary_path_(binary_path)
{
    connect(&proc_, &QProcess::started, this, &WebRtcProcessManager::onProcessStarted);
    connect(&proc_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &WebRtcProcessManager::onProcessFinished);
    connect(&proc_, &QProcess::errorOccurred, this, &WebRtcProcessManager::onProcessError);
    connect(&proc_, &QProcess::readyReadStandardOutput, this, &WebRtcProcessManager::onReadyStdout);
    connect(&proc_, &QProcess::readyReadStandardError, this, &WebRtcProcessManager::onReadyStderr);
}

WebRtcProcessManager::~WebRtcProcessManager() {
    stop();
}

void WebRtcProcessManager::start() {
    if (state_ == Starting || state_ == Running) return;

    setState(Starting);
    retry_count_ = 0;
    proc_.start(binary_path_, QStringList());
}

void WebRtcProcessManager::retryStart() {
    setState(Starting);
    proc_.start(binary_path_, QStringList());
}

void WebRtcProcessManager::stop() {
    if (state_ != Running) {
        if (proc_.state() != QProcess::NotRunning) {
            proc_.kill();
            proc_.waitForFinished(3000);
        }
        setState(Stopped);
        return;
    }
    // Send terminate signal, then kill if needed
    proc_.terminate();
    if (!proc_.waitForFinished(5000)) {
        proc_.kill();
        proc_.waitForFinished(3000);
    }
    setState(Stopped);
}

void WebRtcProcessManager::setState(State s) {
    if (state_ != s) {
        state_ = s;
        emit stateChanged(state_);
    }
}

void WebRtcProcessManager::onProcessStarted() {
    setState(Running);
}

void WebRtcProcessManager::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    if (exitStatus == QProcess::CrashExit) {
        emit daemonLog(QString("Daemon CRASHED (exit code: %1)").arg(exitCode));
    } else {
        emit daemonLog(QString("Daemon exited with code: %1").arg(exitCode));
    }
    setState(Stopped);
}

void WebRtcProcessManager::onProcessError(QProcess::ProcessError error) {
    QString errStr;
    switch (error) {
    case QProcess::FailedToStart: errStr = "FailedToStart"; break;
    case QProcess::Crashed: errStr = "Crashed"; break;
    case QProcess::Timedout: errStr = "Timedout"; break;
    default: errStr = QString("Code %1").arg(error); break;
    }
    emit daemonLog(QString("Daemon process error: %1").arg(errStr));
    if (retry_count_ < kMaxRetries) {
        retry_count_++;
        qWarning() << "WebRTC process error, retry" << retry_count_;
        QTimer::singleShot(2000, this, &WebRtcProcessManager::retryStart);
    } else {
        setState(Error);
        emit daemonLog("WebRTC process failed after retries");
    }
}

void WebRtcProcessManager::onReadyStdout() {
    while (proc_.canReadLine()) {
        QByteArray line = proc_.readLine();
        emit daemonLog(QString::fromUtf8(line).trimmed());
    }
}

void WebRtcProcessManager::onReadyStderr() {
    while (proc_.canReadLine()) {
        QByteArray line = proc_.readLine();
        emit daemonLog(QString::fromUtf8(line).trimmed());
    }
}
