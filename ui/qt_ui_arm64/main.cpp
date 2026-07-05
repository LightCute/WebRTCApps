#include "mainwindow.h"
#include <QApplication>
#include <QSurfaceFormat>
#include <QFile>
#include <QScreen>
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
        app_dir + "/../../material-dark.qss",
        app_dir + "/../material-dark.qss",
        app_dir + "/material-dark.qss",
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
    w.show();

    // show() must come first so the window is assigned to a screen.
    // Then read the screen's availableGeometry (excludes system panel)
    // and resize to fill every available pixel.
    QScreen* screen = w.screen();
    if (screen) {
        QRect geo = screen->availableGeometry();
        qDebug() << "Screen available:" << geo.width() << "x" << geo.height()
                 << "at" << geo.x() << "," << geo.y();
        w.setGeometry(geo);
    } else {
        // Hard fallback: 1024x600 minus ~32px top status bar
        w.setGeometry(0, 32, 1024, 568);
    }

    return a.exec();
}
