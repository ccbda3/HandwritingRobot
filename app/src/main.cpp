#include <QApplication>
#include <QDebug>
#include <QDialog>
#include <QFontDatabase>
#include <QMetaObject>
#include <QStringList>
#include <QTimer>

#include "capturetraindialog.h"
#include "handwriting/bounds.hpp"
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

    // 演示/快照参数：--demo-bounds min_x,min_y,max_x,max_y  --demo-head x,y
    // 用于在没接机器的情况下也能展示已标定的边界线与机头位置。
    using handwriting::bounds::Workspace;
    bool have_demo = false;
    Workspace demo_ws;
    const int bi = arguments.indexOf(QStringLiteral("--demo-bounds"));
    if (bi >= 0 && bi + 1 < arguments.size()) {
        const QStringList parts = arguments.at(bi + 1).split(QLatin1Char(','));
        if (parts.size() == 4) {
            demo_ws.min_x = parts[0].toDouble();
            demo_ws.min_y = parts[1].toDouble();
            demo_ws.max_x = parts[2].toDouble();
            demo_ws.max_y = parts[3].toDouble();
            demo_ws.valid = true;
            have_demo = true;
        }
    }
    double demo_hx = 0.0;
    double demo_hy = 0.0;
    bool have_head = false;
    const int hi = arguments.indexOf(QStringLiteral("--demo-head"));
    if (hi >= 0 && hi + 1 < arguments.size()) {
        const QStringList parts = arguments.at(hi + 1).split(QLatin1Char(','));
        if (parts.size() == 2) {
            demo_hx = parts[0].toDouble();
            demo_hy = parts[1].toDouble();
            have_head = true;
        }
    }
    if (have_demo) {
        window.setDemoWorkspace(demo_ws);
    }
    if (have_head) {
        window.setDemoHead(demo_hx, demo_hy);
    }
    const int ti = arguments.indexOf(QStringLiteral("--demo-text"));
    if (ti >= 0 && ti + 1 < arguments.size()) {
        window.setDemoText(arguments.at(ti + 1));
    }
    if (arguments.contains(QStringLiteral("--demo-style"))) {
        window.setDemoStyle(true);
    }
    if (arguments.contains(QStringLiteral("--demo-model"))) {
        window.setDemoStyleModel(true);
    }

    const bool snapshot_settings =
        snapshot_mode && arguments.contains(QStringLiteral("--snapshot-settings"));
    const bool snapshot_text =
        snapshot_mode && arguments.contains(QStringLiteral("--snapshot-text"));
    const bool snapshot_capture =
        snapshot_mode && arguments.contains(QStringLiteral("--snapshot-capture"));
    const bool snapshot_train =
        snapshot_mode && arguments.contains(QStringLiteral("--snapshot-train"));
    if (snapshot_settings) {
        QMetaObject::invokeMethod(&window, "showSettings", Qt::QueuedConnection);
    }
    if (snapshot_text) {
        QMetaObject::invokeMethod(&window, "editText", Qt::QueuedConnection);
    }
    if (snapshot_capture || snapshot_train) {
        QMetaObject::invokeMethod(&window, "showCaptureTrain", Qt::QueuedConnection);
    }
    if (snapshot_train) {
        QTimer::singleShot(150, &window, [&window] {
            if (auto* dialog = window.captureDialog()) {
                dialog->showTrainingTab();
            }
        });
    }
    if (arguments.contains(QStringLiteral("--demo-capture"))) {
        QMetaObject::invokeMethod(&window, "showCaptureTrain", Qt::QueuedConnection);
        QTimer::singleShot(200, &window, [&window] {
            if (auto* dialog = window.captureDialog()) {
                dialog->saveDemoCapture();
                qInfo().noquote() << "demo capture:" << dialog->lastExportPath();
            }
        });
    }

    if (snapshot_mode) {
        const QString path = arguments.at(index + 1);
        // 等一拍让延迟刷新定时器跑完、布局稳定下来
        QTimer::singleShot(600, &window,
                           [&window, path, snapshot_settings, snapshot_text,
                            snapshot_capture, snapshot_train]() {
                               QWidget* target = &window;
                               if ((snapshot_capture || snapshot_train) &&
                                   window.captureDialog() != nullptr) {
                                   target = window.captureDialog();
                               } else if (snapshot_text &&
                                          window.textDialog() != nullptr) {
                                   target = window.textDialog();
                               } else if (snapshot_settings &&
                                          window.settingsDialog() != nullptr) {
                                   target = window.settingsDialog();
                               }
                               const bool ok = target->grab().save(path);
                               QApplication::exit(ok ? 0 : 1);
                           });
    }

    return QApplication::exec();
}
