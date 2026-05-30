#include <QApplication>
#include "mainwindow.h"

int main(int argc, char *argv[]) {
    // Force X11 (xcb) backend for Qt to prevent Wayland protocol error 71 crashes
    // which occur primarily on NVIDIA/Wayland configurations
    qputenv("QT_QPA_PLATFORM", "xcb");

    QApplication app(argc, argv);
    app.setApplicationName("Arthur Control Center");
    app.setApplicationVersion("3.5.0");

    MainWindow w;
    w.resize(820, 520);
    w.show();

    return app.exec();
}
