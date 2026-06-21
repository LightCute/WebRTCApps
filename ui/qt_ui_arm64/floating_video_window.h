#pragma once
#include <QWidget>

class GlVideoWidget;

class FloatingVideoWindow : public QWidget
{
    Q_OBJECT

public:
    explicit FloatingVideoWindow(QWidget* parent = nullptr);
    ~FloatingVideoWindow() override;

    void setFrame(const QImage& frame);
    void closeWindow();

private:
    GlVideoWidget* video_widget_;
};
