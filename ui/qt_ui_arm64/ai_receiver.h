#pragma once
#include <QObject>
#include <QThread>
#include <QByteArray>
#include <QVector>
#include <QString>
#include <QMetaType>
#include <atomic>

struct Detection {
    int cls_id = 0;
    QString label;
    int left = 0, top = 0, right = 0, bottom = 0;
    float conf = 0.0f;
};
Q_DECLARE_METATYPE(Detection)

class AiReceiver : public QObject
{
    Q_OBJECT
public:
    explicit AiReceiver(const QString& socketPath, QObject* parent = nullptr);
    ~AiReceiver() override;
    void start();
    void stop();

signals:
    void detectionsReady(QVector<Detection> detections);

private:
    void run();
    void parse(const QByteArray& json);

    QString socket_path_;
    std::atomic<bool> running_{false};
    QThread* thread_ = nullptr;
    QByteArray buf_;
};
