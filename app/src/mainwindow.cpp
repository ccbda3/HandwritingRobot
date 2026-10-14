#include "mainwindow.h"

#include <QAction>
#include <QFileDialog>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QProcess>
#include <QStatusBar>
#include <QWidget>

#include <algorithm>

namespace {
constexpr int kMargin = 24;
constexpr qreal kMinSpan = 1.0;
}  // namespace

StrokeCanvas::StrokeCanvas(QWidget* parent) : QWidget(parent) {
    setMinimumSize(360, 360);
    setAutoFillBackground(true);
}

void StrokeCanvas::setStrokes(const QVector<QVector<QPointF>>& strokes) {
    strokes_ = strokes;
    update();
}

void StrokeCanvas::clear() {
    strokes_.clear();
    update();
}

QSize StrokeCanvas::sizeHint() const {
    return {480, 480};
}

QRectF StrokeCanvas::contentBounds() const {
    // 注意不能用 QRectF::isNull() 判断「是否已初始化」：
    // 单点包围盒的宽高都是 0，同样满足 isNull()，会导致后续点覆盖第一个点。
    bool initialised = false;
    double min_x = 0.0;
    double min_y = 0.0;
    double max_x = 0.0;
    double max_y = 0.0;

    for (const auto& stroke : strokes_) {
        for (const auto& point : stroke) {
            if (!initialised) {
                min_x = max_x = point.x();
                min_y = max_y = point.y();
                initialised = true;
                continue;
            }
            min_x = std::min(min_x, point.x());
            min_y = std::min(min_y, point.y());
            max_x = std::max(max_x, point.x());
            max_y = std::max(max_y, point.y());
        }
    }

    if (!initialised) {
        return {};
    }
    return QRectF(QPointF(min_x, min_y), QPointF(max_x, max_y));
}

void StrokeCanvas::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), Qt::white);

    if (strokes_.isEmpty()) {
        painter.setPen(QColor(0x99, 0x99, 0x99));
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("暂无轨迹\n先在采集器里写入笔迹数据"));
        return;
    }

    const QRectF bounds = contentBounds();
    const qreal span = std::max({bounds.width(), bounds.height(), kMinSpan});
    const qreal scale = (std::min(width(), height()) - 2.0 * kMargin) / span;
    const QPointF center = bounds.center();

    painter.translate(width() / 2.0, height() / 2.0);
    painter.scale(scale, scale);
    painter.translate(-center.x(), -center.y());

    QPen pen(QColor(0x14, 0x14, 0x14));
    pen.setWidthF(1.6 / scale);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);

    for (const auto& stroke : strokes_) {
        if (stroke.size() == 1) {
            painter.drawPoint(stroke.first());
            continue;
        }
        painter.drawPolyline(stroke.constData(), static_cast<int>(stroke.size()));
    }
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("HandwritingRobot Studio"));
    resize(900, 700);

    buildLayout();
    buildActions();
    startBackend();
}

MainWindow::~MainWindow() {
    stopBackend();
}

void MainWindow::buildLayout() {
    canvas_ = new StrokeCanvas(this);
    setCentralWidget(canvas_);

    backend_label_ = new QLabel(QStringLiteral("后端：未连接"), this);
    hint_label_ = new QLabel(QStringLiteral("推理子进程尚未接入，当前仅显示前端骨架"), this);
    statusBar()->addWidget(backend_label_);
    statusBar()->addPermanentWidget(hint_label_);
}

void MainWindow::buildActions() {
    QMenu* file_menu = menuBar()->addMenu(QStringLiteral("文件(&F)"));

    QAction* export_action = file_menu->addAction(QStringLiteral("导出 G 代码(&E)..."));
    export_action->setShortcut(QKeySequence::Save);
    connect(export_action, &QAction::triggered, this, &MainWindow::exportGcode);

    file_menu->addSeparator();
    QAction* quit_action = file_menu->addAction(QStringLiteral("退出(&Q)"));
    quit_action->setShortcut(QKeySequence::Quit);
    connect(quit_action, &QAction::triggered, this, &QWidget::close);

    QMenu* backend_menu = menuBar()->addMenu(QStringLiteral("推理后端(&B)"));
    connect(backend_menu->addAction(QStringLiteral("重新连接")), &QAction::triggered, this,
            &MainWindow::startBackend);
    connect(backend_menu->addAction(QStringLiteral("断开")), &QAction::triggered, this,
            &MainWindow::stopBackend);
}

void MainWindow::startBackend() {
    if (backend_ != nullptr) {
        return;
    }

    backend_ = new QProcess(this);
    connect(backend_, &QProcess::readyReadStandardOutput, this, &MainWindow::readBackendStdout);
    connect(backend_, &QProcess::readyReadStandardError, this, &MainWindow::readBackendStderr);
    connect(backend_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int exit_code, QProcess::ExitStatus) { onBackendFinished(exit_code); });

    // 健康检查：确认 Python 环境里能 import 到 handwriting 包。
    // 后续替换为常驻子进程 + 长度前缀帧协议。
    const QString program = QStringLiteral("python");
    const QStringList arguments = {QStringLiteral("-c"),
                                   QStringLiteral("import handwriting;"
                                                  "print(handwriting.__version__)")};

    setBackendStatus(QStringLiteral("后端：连接中…"), false);
    backend_->start(program, arguments);
}

void MainWindow::stopBackend() {
    if (backend_ == nullptr) {
        return;
    }
    backend_->disconnect(this);
    if (backend_->state() != QProcess::NotRunning) {
        backend_->terminate();
        if (!backend_->waitForFinished(3000)) {
            backend_->kill();
        }
    }
    backend_->deleteLater();
    backend_ = nullptr;
    setBackendStatus(QStringLiteral("后端：已断开"), false);
}

void MainWindow::readBackendStdout() {
    if (backend_ == nullptr) {
        return;
    }
    const QString output = QString::fromUtf8(backend_->readAllStandardOutput()).trimmed();
    if (!output.isEmpty()) {
        setBackendStatus(QStringLiteral("后端：handwriting %1").arg(output), true);
    }
}

void MainWindow::readBackendStderr() {
    if (backend_ == nullptr) {
        return;
    }
    const QString output = QString::fromUtf8(backend_->readAllStandardError()).trimmed();
    if (!output.isEmpty()) {
        qWarning("backend stderr: %s", qPrintable(output));
    }
}

void MainWindow::onBackendFinished(int exit_code) {
    if (exit_code != 0) {
        setBackendStatus(QStringLiteral("后端：启动失败（退出码 %1），请检查 Python 环境").arg(exit_code),
                         false);
    }
}

void MainWindow::setBackendStatus(const QString& text, bool healthy) {
    backend_label_->setText(text);
    backend_label_->setStyleSheet(healthy ? QStringLiteral("color:#1d7a4d;")
                                          : QStringLiteral("color:#a15c00;"));
}

void MainWindow::exportGcode() {
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出 G 代码"), QStringLiteral("handwriting.nc"),
        QStringLiteral("G 代码 (*.nc *.gcode *.tap)"));
    if (path.isEmpty()) {
        return;
    }

    // TODO(阶段 1)：接上 Python 侧 strokes_to_gcode，这里先给出明确的未实现提示。
    QMessageBox::information(
        this, QStringLiteral("尚未接入"),
        QStringLiteral("G 代码生成位于 Python 侧（handwriting.gcode），"
                       "待子进程协议接通后由后端返回文本。"));
}
