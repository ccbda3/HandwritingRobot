#include <QApplication>
#include <QStringList>
#include <QTimer>

#include "mainwindow.h"

// 无头快照：`handwriting-studio --snapshot out.png`
//
// 配合 QT_QPA_PLATFORM=offscreen 使用，可以在没有显示器的环境里
// 把界面渲染成图片。开发期用来确认布局没写崩，CI 里用来做视觉回归。
int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("HandwritingRobot Studio"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QApplication::setOrganizationName(QStringLiteral("HandwritingRobot"));

    MainWindow window;
    window.show();

    const QStringList arguments = QApplication::arguments();
    const int index = arguments.indexOf(QStringLiteral("--snapshot"));
    if (index >= 0 && index + 1 < arguments.size()) {
        const QString path = arguments.at(index + 1);
        // 等一拍让延迟刷新定时器跑完、布局稳定下来
        QTimer::singleShot(600, &window, [&window, path]() {
            const bool ok = window.grab().save(path);
            QApplication::exit(ok ? 0 : 1);
        });
    }

    return QApplication::exec();
}
