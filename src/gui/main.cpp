// SPDX-License-Identifier: MIT
#include "mainwindow.h"
#include <QApplication>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("USBUtils");
    QApplication::setOrganizationName("AldySan");
    MainWindow w;
    w.show();
    return app.exec();
}
