#include <QApplication>
#include <QFontDatabase>
#include <QStringList>
#include <QTimer>

#include "mainwindow.h"

namespace {

// 为无头快照注册一个系统中文字体。
//
// Qt6 不再自带字体，而 offscreen 平台用的是基础字体引擎（不走 Windows 字体数据库），
// 因此找不到任何字体，界面文字会全部渲染成方框 —— 只有方框的截图看不出布局对不对。
// 真实的 windows 平台走系统字体数据库，不受影响，所以这个补丁只在快照模式下生效。
void register_snapshot_fonts() {
#ifdef Q_OS_WIN
    const char* candidates[] = {
        "C:/Windows/Fonts/msyh.ttc",    // 微软雅黑
        "C:/Windows/Fonts/msyh.ttf",
        "C:/Windows/Fonts/simhei.ttf",  // 黑体
        "C:/Windows/Fonts/simsun.ttc",  // 宋体
        "C:/Windows/Fonts/arial.ttf",
    };
    for (const char* path : candidates) {
        if (QFontDatabase::addApplicationFont(QString::fromLatin1(path)) >= 0) {
            break;
        }
    }
#endif
}

}  // namespace

// 无头快照：`handwriting-studio --snapshot out.png`
//
// 配合 QT_QPA_PLATFORM=offscreen 使用，可以在没有显示器的环境里
// 把界面渲染成图片。开发期用来确认布局没写崩，CI 里用来做视觉回归。
int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("HandwritingRobot Studio"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QApplication::setOrganizationName(QStringLiteral("HandwritingRobot"));

    const QStringList arguments = QApplication::arguments();
    const int index = arguments.indexOf(QStringLiteral("--snapshot"));
    const bool snapshot_mode = index >= 0 && index + 1 < arguments.size();

    if (snapshot_mode) {
        register_snapshot_fonts();
    }

    MainWindow window;
    window.show();

    if (snapshot_mode) {
        const QString path = arguments.at(index + 1);
        // 等一拍让延迟刷新定时器跑完、布局稳定下来
        QTimer::singleShot(600, &window, [&window, path]() {
            const bool ok = window.grab().save(path);
            QApplication::exit(ok ? 0 : 1);
        });
    }

    return QApplication::exec();
}
