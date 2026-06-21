#pragma once
#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QImage>
#include <QPainter>
#include <QMutex>
#include <QVector>
#include "ai_receiver.h"

class GlVideoWidget : public QOpenGLWidget, protected QOpenGLFunctions
{
    Q_OBJECT

public:
    explicit GlVideoWidget(QWidget* parent = nullptr);
    ~GlVideoWidget() override;

public slots:
    void setFrame(QImage frame);
    void setFrameInfo(const QString& info);
    void setDetections(QVector<Detection> detections);

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int w, int h) override;

private:
    void initShader();
    void uploadTexture(const QImage& frame);

    QOpenGLShaderProgram* program_ = nullptr;
    GLuint texture_id_ = 0;
    int tex_w_ = 0;
    int tex_h_ = 0;
    bool first_upload_ = true;

    QImage pending_frame_;
    bool frame_dirty_ = false;
    QMutex mutex_;
    QString frame_info_;
    QVector<Detection> detections_;
};
