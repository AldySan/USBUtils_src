// SPDX-License-Identifier: MIT
#include "mainwindow.h"
#include <QApplication>
#include <QIcon>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("USBUtils");
    QApplication::setOrganizationName("AldySan");
    QApplication::setApplicationVersion(USBUTILS_VERSION);
    // Compiled-in icon (res/usbutils.qrc) — window, taskbar, alt-tab on
    // every platform; the exe-file icon on Windows comes from res/usbutils.rc.
    QIcon appIcon(":/res/icons/usbutils_256.png");
    if (!appIcon.isNull()) {
        app.setWindowIcon(appIcon);
    } else {
        // fallback for theme installs (icon named "usbutils")
        app.setWindowIcon(QIcon::fromTheme("usbutils"));
    }
    MainWindow w;
    w.show();
    return app.exec();
}
