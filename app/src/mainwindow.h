// Qt 桌面前端主窗口。
//
// 架构约定（见 docs/adr/0003-qt-frontend-python-subprocess.md）：
//   本进程只负责交互与渲染，不做任何 AI 推理。
//   推理由独立的 Python 子进程承担，通过标准输入输出以长度前缀二进制帧通信，
//   避免 UI 阻塞，也规避在 C++ 中嵌入 Python 解释器带来的内存管理问题。

#pragma once

#include <QMainWindow>
#include <QString>
#include <QVector>

class QLabel;
class QProcess;

// 简单的笔画画布：只做坐标变换与绘制，不含业务逻辑。
class StrokeCanvas : public QWidget {
    Q_OBJECT

public:
    explicit StrokeCanvas(QWidget* parent = nullptr);

    // 传入手写机器人坐标系下的笔迹（单位 mm），按书写顺序绘制。
    void setStrokes(const QVector<QVector<QPointF>>& strokes);
    void clear();

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QVector<QVector<QPointF>> strokes_;
    QRectF contentBounds() const;
};

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void startBackend();
    void stopBackend();
    void readBackendStdout();
    void readBackendStderr();
    void onBackendFinished(int exitCode);
    void exportGcode();

private:
    void buildActions();
    void buildLayout();
    void setBackendStatus(const QString& text, bool healthy);

    StrokeCanvas* canvas_ = nullptr;
    QLabel* backend_label_ = nullptr;
    QLabel* hint_label_ = nullptr;
    QProcess* backend_ = nullptr;
};
