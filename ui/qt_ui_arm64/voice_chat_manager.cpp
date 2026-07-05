// voice_chat_manager.cpp
#include "voice_chat_manager.h"
#include <QCoreApplication>
#include <QDebug>

VoiceChatManager::VoiceChatManager(QObject* parent)
    : QObject(parent)
{
    connect(&proc_, &QProcess::readyReadStandardOutput, this, [this]() {
        while (proc_.canReadLine())
            emit daemonLog(QString::fromUtf8(proc_.readLine()).trimmed());
    });
    connect(&proc_, &QProcess::readyReadStandardError, this, [this]() {
        while (proc_.canReadLine())
            emit daemonLog(QString::fromUtf8(proc_.readLine()).trimmed());
    });
}

VoiceChatManager::~VoiceChatManager()
{
    stop();
}

void VoiceChatManager::start(const QString& socket_path)
{
    if (proc_.state() != QProcess::NotRunning) return;
    QString script_dir = QCoreApplication::applicationDirPath();
    proc_.setWorkingDirectory(script_dir);
    proc_.start("/usr/bin/python3", {"voice_chat.py", "--socket", socket_path});
}

void VoiceChatManager::stop()
{
    if (proc_.state() == QProcess::NotRunning) return;
    proc_.terminate();
    if (!proc_.waitForFinished(3000)) {
        proc_.kill();
        proc_.waitForFinished(2000);
    }
}
