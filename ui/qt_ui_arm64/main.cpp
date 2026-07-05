#include "mainwindow.h"
#include <QApplication>
#include <QSurfaceFormat>
#include <QFile>
#include <QDebug>

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "xcb");

    QSurfaceFormat fmt;
    fmt.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(fmt);

    QApplication a(argc, argv);

    // Force Fusion style for consistent QSS rendering
    a.setStyle("Fusion");

    // Load Material Dark QSS stylesheet
    QString app_dir = a.applicationDirPath();
    QStringList qss_paths = {
        app_dir + "/../../material-dark.qss",       // build dir (ui_rk/build-arm64 -> webrtc_monitor/)
        app_dir + "/../material-dark.qss",          // alt build layout
        app_dir + "/material-dark.qss",             // deployed alongside binary
    };
    bool qss_loaded = false;
    for (const auto& path : qss_paths) {
        if (QFile::exists(path)) {
            QFile f(path);
            if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                a.setStyleSheet(QString::fromUtf8(f.readAll()));
                f.close();
                qss_loaded = true;
                qDebug() << "QSS loaded from:" << path;
                break;
            }
        }
    }
    if (!qss_loaded) {
        qDebug() << "QSS not found! Searched:" << qss_paths;
    }

    MainWindow w;
    w.showFullScreen();
    return a.exec();
}
