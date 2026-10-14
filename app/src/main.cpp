#include <QApplication>

#include "mainwindow.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("HandwritingRobot Studio"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QApplication::setOrganizationName(QStringLiteral("HandwritingRobot"));

    MainWindow window;
    window.show();

    return QApplication::exec();
}
