#include <QApplication>
#include "mainwindow.h"

int main(int argc, char *argv[]) {
    // Force X11 (xcb) backend for Qt only when Wayland is not active,
    // to prevent Wayland protocol error 71 crashes on X11 fallback.
    if (qgetenv("WAYLAND_DISPLAY").isEmpty()) {
        qputenv("QT_QPA_PLATFORM", "xcb");
    }

    QApplication app(argc, argv);
    app.setApplicationName("Arthur Control Center");
    app.setApplicationVersion("3.5.0");

    MainWindow w;
    w.resize(820, 520);
    w.show();

    return app.exec();
}
