#include "floating_video_window.h"
#include "gl_video_widget.h"
#include <QVBoxLayout>
#include <QGuiApplication>
#include <QScreen>

FloatingVideoWindow::FloatingVideoWindow(QWidget* parent)
    : QWidget(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool)
{
    Q_UNUSED(parent);
    setFixedSize(404, 308);
    setAttribute(Qt::WA_ShowWithoutActivating);

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);

    video_widget_ = new GlVideoWidget(this);
    video_widget_->setMinimumSize(400, 300);
    video_widget_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    layout->addWidget(video_widget_);

    // Fixed at top-right corner of screen
    const auto screens = QGuiApplication::screens();
    if (!screens.isEmpty()) {
        QRect screen = screens.first()->availableGeometry();
        move(screen.right() - width() - 20, screen.top() + 60);
    }
}

FloatingVideoWindow::~FloatingVideoWindow() = default;

void FloatingVideoWindow::setFrame(const QImage& frame) {
    video_widget_->setFrame(frame);
}

void FloatingVideoWindow::closeWindow() {
    hide();
}
