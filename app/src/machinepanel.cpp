#include "machinepanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

using handwriting::grbl::Controller;
using handwriting::grbl::Options;
using handwriting::grbl::ProgramBounds;
using handwriting::grbl::State;
using handwriting::serial::PortInfo;

namespace {

constexpr int kPollIntervalMs = 20;       ///< 串口轮询：要足够快，否则缓冲会空转
constexpr int kStatusIntervalMs = 250;    ///< 状态查询间隔
constexpr int kHandshakeTimeoutMs = 2500; ///< 打开串口后等 GRBL 回话的时限

QString format_mm(double value) {
    return QString::number(value, 'f', 2);
}

}  // namespace

MachinePanel::MachinePanel(QWidget* parent) : QWidget(parent) {
    controller_ = std::make_unique<Controller>(transport_);

    buildUi();

    poll_timer_ = new QTimer(this);
    poll_timer_->setInterval(kPollIntervalMs);
    connect(poll_timer_, &QTimer::timeout, this, &MachinePanel::onPoll);

    status_timer_ = new QTimer(this);
    status_timer_->setInterval(kStatusIntervalMs);
    connect(status_timer_, &QTimer::timeout, this, [this] {
        if (controller_->connected()) {
            controller_->request_status();
        }
    });

    handshake_timer_ = new QTimer(this);
    handshake_timer_->setSingleShot(true);
    handshake_timer_->setInterval(kHandshakeTimeoutMs);
    connect(handshake_timer_, &QTimer::timeout, this, &MachinePanel::onHandshakeTimeout);

    refreshPorts();
    updateConnectionUi();
    updateProgramUi();
}

MachinePanel::~MachinePanel() {
    if (controller_) {
        controller_->disconnect();
    }
}

// ---------------------------------------------------------------------------
// 界面
// ---------------------------------------------------------------------------

void MachinePanel::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 6, 8, 6);
    root->setSpacing(6);

    // ---- 第一行：连接 ----
    auto* connection = new QHBoxLayout;
    connection->addWidget(new QLabel(QStringLiteral("串口")));

    port_combo_ = new QComboBox;
    port_combo_->setMinimumWidth(220);
    connection->addWidget(port_combo_);

    refresh_button_ = new QPushButton(QStringLiteral("刷新"));
    connect(refresh_button_, &QPushButton::clicked, this, &MachinePanel::refreshPorts);
    connection->addWidget(refresh_button_);

    connection->addWidget(new QLabel(QStringLiteral("波特率")));
    baud_combo_ = new QComboBox;
    for (const int rate : handwriting::serial::kBaudRates) {
        baud_combo_->addItem(QString::number(rate), rate);
    }
    connection->addWidget(baud_combo_);

    connect_button_ = new QPushButton(QStringLiteral("连接"));
    connect_button_->setMinimumWidth(90);
    connect(connect_button_, &QPushButton::clicked, this, &MachinePanel::toggleConnection);
    connection->addWidget(connect_button_);

    connection_label_ = new QLabel(QStringLiteral("未连接"));
    connection->addWidget(connection_label_);
    connection->addStretch();
    root->addLayout(connection);

    // ---- 第二行：状态 ----
    auto* status_row = new QHBoxLayout;
    state_label_ = new QLabel(QStringLiteral("状态 —"));
    position_label_ = new QLabel(QStringLiteral("位置 —"));
    limits_label_ = new QLabel(QStringLiteral("行程 未知"));
    for (QLabel* label : {state_label_, position_label_, limits_label_}) {
        label->setMinimumWidth(180);
        status_row->addWidget(label);
    }
    status_row->addStretch();
    root->addLayout(status_row);

    // ---- 第三行：手动控制 + 程序控制 ----
    auto* columns = new QHBoxLayout;
    columns->setSpacing(10);

    auto* manual = new QGroupBox(QStringLiteral("手动控制"));
    auto* manual_grid = new QGridLayout(manual);
    manual_grid->setSpacing(4);

    home_button_ = new QPushButton(QStringLiteral("回零 $H"));
    unlock_button_ = new QPushButton(QStringLiteral("解锁 $X"));
    origin_button_ = new QPushButton(QStringLiteral("设零点"));
    settings_button_ = new QPushButton(QStringLiteral("读参数 $$"));
    pen_up_button_ = new QPushButton(QStringLiteral("抬笔"));
    pen_down_button_ = new QPushButton(QStringLiteral("落笔"));

    connect(home_button_, &QPushButton::clicked, this, &MachinePanel::onHome);
    connect(unlock_button_, &QPushButton::clicked, this, &MachinePanel::onUnlock);
    connect(origin_button_, &QPushButton::clicked, this, &MachinePanel::onSetOrigin);
    connect(settings_button_, &QPushButton::clicked, this, &MachinePanel::onReadSettings);
    connect(pen_up_button_, &QPushButton::clicked, this, &MachinePanel::onPenUp);
    connect(pen_down_button_, &QPushButton::clicked, this, &MachinePanel::onPenDown);

    manual_grid->addWidget(home_button_, 0, 0);
    manual_grid->addWidget(unlock_button_, 0, 1);
    manual_grid->addWidget(origin_button_, 0, 2);
    manual_grid->addWidget(settings_button_, 0, 3);
    manual_grid->addWidget(pen_up_button_, 1, 0);
    manual_grid->addWidget(pen_down_button_, 1, 1);

    manual_grid->addWidget(new QLabel(QStringLiteral("步长")), 2, 0);
    jog_step_ = new QDoubleSpinBox;
    jog_step_->setRange(0.1, 100.0);
    jog_step_->setValue(1.0);
    jog_step_->setSuffix(QStringLiteral(" mm"));
    manual_grid->addWidget(jog_step_, 2, 1);

    manual_grid->addWidget(new QLabel(QStringLiteral("进给")), 2, 2);
    jog_feed_ = new QDoubleSpinBox;
    jog_feed_->setRange(10.0, 12000.0);
    jog_feed_->setValue(1000.0);
    jog_feed_->setSuffix(QStringLiteral(" mm/min"));
    manual_grid->addWidget(jog_feed_, 2, 3);

    const auto add_jog = [this, manual_grid](const QString& text, double dx, double dy, double dz,
                                             int row, int column) {
        auto* button = new QPushButton(text);
        connect(button, &QPushButton::clicked, this,
                [this, dx, dy, dz] { jog(dx, dy, dz); });
        manual_grid->addWidget(button, row, column);
    };
    add_jog(QStringLiteral("X−"), -1, 0, 0, 3, 0);
    add_jog(QStringLiteral("X+"), 1, 0, 0, 3, 1);
    add_jog(QStringLiteral("Y−"), 0, -1, 0, 3, 2);
    add_jog(QStringLiteral("Y+"), 0, 1, 0, 3, 3);
    add_jog(QStringLiteral("Z−"), 0, 0, -1, 4, 0);
    add_jog(QStringLiteral("Z+"), 0, 0, 1, 4, 1);

    columns->addWidget(manual, 1);

    auto* program = new QGroupBox(QStringLiteral("书写程序"));
    auto* program_grid = new QGridLayout(program);
    program_grid->setSpacing(4);

    start_button_ = new QPushButton(QStringLiteral("开始书写"));
    pause_button_ = new QPushButton(QStringLiteral("暂停"));
    resume_button_ = new QPushButton(QStringLiteral("继续"));
    abort_button_ = new QPushButton(QStringLiteral("中止"));
    start_button_->setMinimumHeight(34);

    connect(start_button_, &QPushButton::clicked, this, &MachinePanel::onStart);
    connect(pause_button_, &QPushButton::clicked, this, &MachinePanel::onPause);
    connect(resume_button_, &QPushButton::clicked, this, &MachinePanel::onResume);
    connect(abort_button_, &QPushButton::clicked, this, &MachinePanel::onAbort);

    program_grid->addWidget(start_button_, 0, 0, 1, 2);
    program_grid->addWidget(pause_button_, 0, 2);
    program_grid->addWidget(resume_button_, 0, 3);
    program_grid->addWidget(abort_button_, 0, 4);

    progress_bar_ = new QProgressBar;
    progress_bar_->setRange(0, 1000);
    progress_bar_->setValue(0);
    progress_bar_->setTextVisible(false);
    program_grid->addWidget(progress_bar_, 1, 0, 1, 5);

    progress_label_ = new QLabel(QStringLiteral("—"));
    program_grid->addWidget(progress_label_, 2, 0, 1, 5);

    program_label_ = new QLabel(QStringLiteral("—"));
    program_label_->setStyleSheet(QStringLiteral("color:#8a8a96;"));
    program_grid->addWidget(program_label_, 3, 0, 1, 5);

    skip_safety_ = new QCheckBox(QStringLiteral("跳过超程检查（危险）"));
    program_grid->addWidget(skip_safety_, 4, 0, 1, 5);

    columns->addWidget(program, 2);
    root->addLayout(columns);

    // ---- 日志 ----
    log_view_ = new QPlainTextEdit;
    log_view_->setReadOnly(true);
    log_view_->setMaximumBlockCount(300);
    log_view_->setMaximumHeight(90);
    log_view_->setPlaceholderText(QStringLiteral("通信日志"));
    root->addWidget(log_view_);
}

// ---------------------------------------------------------------------------
// 连接
// ---------------------------------------------------------------------------

void MachinePanel::refreshPorts() {
    const QString previous = port_combo_->currentData().toString();
    port_combo_->clear();

    const std::vector<PortInfo> ports = handwriting::serial::list_ports();
    for (const PortInfo& port : ports) {
        const QString label = port.description.empty()
                                  ? QString::fromStdString(port.name)
                                  : QString::fromStdString(port.description);
        port_combo_->addItem(label, QString::fromStdString(port.name));
    }

    if (ports.empty()) {
        port_combo_->addItem(QStringLiteral("（未发现串口）"), QString());
    } else {
        const int index = port_combo_->findData(previous);
        if (index >= 0) {
            port_combo_->setCurrentIndex(index);
        }
    }
    appendLog(QStringLiteral("发现 %1 个串口").arg(ports.size()));
}

void MachinePanel::toggleConnection() {
    if (controller_->connected()) {
        controller_->disconnect();
        poll_timer_->stop();
        status_timer_->stop();
        handshake_timer_->stop();
        appendLog(QStringLiteral("已断开"));
        updateConnectionUi();
        return;
    }

    const QString port = port_combo_->currentData().toString();
    if (port.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法连接"),
                             QStringLiteral("没有可用的串口。请插上机器并点「刷新」。"));
        return;
    }
    const int baud = baud_combo_->currentData().toInt();

    controller_->clear_log();
    handshake_warned_ = false;
    reported_errors_ = 0;
    reported_messages_ = 0;

    if (!controller_->connect(port.toStdString(), baud)) {
        QMessageBox::critical(this, QStringLiteral("连接失败"),
                              QString::fromStdString(controller_->last_error()));
        appendLog(QStringLiteral("连接失败：%1").arg(QString::fromStdString(controller_->last_error())));
        updateConnectionUi();
        return;
    }

    appendLog(QStringLiteral("已打开 %1 @ %2，等待 GRBL 响应…").arg(port).arg(baud));
    poll_timer_->start();
    status_timer_->start();
    handshake_timer_->start();
    updateConnectionUi();
}

void MachinePanel::onHandshakeTimeout() {
    if (!controller_->connected() || controller_->handshake_ok()) {
        return;
    }
    controller_->mark_handshake_failed();
    handshake_warned_ = true;
    appendLog(QString::fromStdString(controller_->last_error()));
    QMessageBox::warning(this, QStringLiteral("没有收到 GRBL 响应"),
                         QString::fromStdString(controller_->last_error()));
}

void MachinePanel::onPoll() {
    if (!controller_->connected()) {
        return;
    }

    controller_->poll();

    // 只倒出新增的错误与消息，避免重复刷屏
    const auto& errors = controller_->errors();
    for (std::size_t i = reported_errors_; i < errors.size(); ++i) {
        appendLog(QStringLiteral("[错误] %1").arg(QString::fromStdString(errors[i])));
    }
    reported_errors_ = errors.size();

    const auto& messages = controller_->messages();
    for (std::size_t i = reported_messages_; i < messages.size(); ++i) {
        appendLog(QString::fromStdString(messages[i]));
    }
    reported_messages_ = messages.size();

    if (controller_->finished() && start_button_->isEnabled()) {
        appendLog(QStringLiteral("书写完成"));
    }

    updateStatusUi();
    updateProgramUi();
}

bool MachinePanel::connected() const {
    return controller_ && controller_->connected();
}

void MachinePanel::updateConnectionUi() {
    const bool is_connected = controller_->connected();
    connect_button_->setText(is_connected ? QStringLiteral("断开") : QStringLiteral("连接"));
    connection_label_->setText(is_connected ? QStringLiteral("已连接")
                                            : QStringLiteral("未连接"));
    connection_label_->setStyleSheet(is_connected ? QStringLiteral("color:#3ddc97;")
                                                  : QStringLiteral("color:#8a8a96;"));
    port_combo_->setEnabled(!is_connected);
    baud_combo_->setEnabled(!is_connected);
    refresh_button_->setEnabled(!is_connected);

    setEnabledWhenConnected(is_connected);
    updateProgramUi();
}

void MachinePanel::setEnabledWhenConnected(bool enabled) {
    for (QPushButton* button :
         {home_button_, unlock_button_, origin_button_, settings_button_, pen_up_button_,
          pen_down_button_, start_button_, pause_button_, resume_button_, abort_button_}) {
        button->setEnabled(enabled);
    }
    jog_step_->setEnabled(enabled);
    jog_feed_->setEnabled(enabled);
    skip_safety_->setEnabled(enabled);
}

void MachinePanel::updateStatusUi() {
    const auto& status = controller_->status();
    const auto& limits = controller_->limits();

    state_label_->setText(QStringLiteral("状态 %1").arg(
        QString::fromUtf8(handwriting::grbl::to_string(status.state))));
    if (status.state == State::Alarm) {
        state_label_->setStyleSheet(QStringLiteral("color:#ff6b6b;font-weight:bold;"));
    } else if (status.state == State::Run) {
        state_label_->setStyleSheet(QStringLiteral("color:#3ddc97;"));
    } else {
        state_label_->setStyleSheet(QString());
    }

    if (status.has_work_position) {
        position_label_->setText(QStringLiteral("工件坐标 X %1  Y %2  Z %3")
                                     .arg(format_mm(status.work[0]))
                                     .arg(format_mm(status.work[1]))
                                     .arg(format_mm(status.work[2])));
    } else if (status.has_machine_position) {
        position_label_->setText(QStringLiteral("机床坐标 X %1  Y %2  Z %3")
                                     .arg(format_mm(status.machine[0]))
                                     .arg(format_mm(status.machine[1]))
                                     .arg(format_mm(status.machine[2])));
    } else {
        position_label_->setText(QStringLiteral("位置 —"));
    }

    if (limits.travel_known()) {
        limits_label_->setText(QStringLiteral("行程 X %1  Y %2  Z %3 mm")
                                   .arg(format_mm(limits.max_x))
                                   .arg(format_mm(limits.max_y))
                                   .arg(format_mm(limits.max_z)));
    } else {
        limits_label_->setText(QStringLiteral("行程 未知（点「读参数」）"));
    }
}

// ---------------------------------------------------------------------------
// 程序
// ---------------------------------------------------------------------------

void MachinePanel::setProgramProvider(std::function<std::string()> provider) {
    program_provider_ = std::move(provider);
    notifyProgramChanged();
}

void MachinePanel::notifyProgramChanged() {
    // 只有这里才真正向 MainWindow 要程序。生成一页 G 代码约 40ms，
    // 而 onPoll 每 20ms 跑一次 —— 若在 updateProgramUi 里调用 provider，
    // 界面会被彻底卡死。
    if (program_provider_ == nullptr) {
        cached_program_.clear();
        cached_bounds_ = {};
    } else {
        cached_program_ = program_provider_();
        cached_bounds_ = handwriting::grbl::analyze(cached_program_);
    }
    updateProgramUi();
}

void MachinePanel::updateProgramUi() {
    const std::string& program = cached_program_;
    const ProgramBounds& bounds = cached_bounds_;

    if (program.empty() || !bounds.valid) {
        program_label_->setText(QStringLiteral("没有可发送的内容"));
        start_button_->setEnabled(false);
        progress_bar_->setValue(0);
        progress_label_->setText(QStringLiteral("—"));
        return;
    }

    program_label_->setText(
        QStringLiteral("待发送：%1 行 · 范围 X %2~%3  Y %4~%5 mm · 最大进给 %6")
            .arg(bounds.line_count)
            .arg(format_mm(bounds.min_x))
            .arg(format_mm(bounds.max_x))
            .arg(format_mm(bounds.min_y))
            .arg(format_mm(bounds.max_y))
            .arg(format_mm(bounds.max_feed)));

    const auto& progress = controller_->progress();
    if (progress.total > 0) {
        progress_bar_->setValue(static_cast<int>(progress.fraction() * 1000.0));
        progress_label_->setText(QStringLiteral("已确认 %1 / %2 行 · 在途 %3 · 队列 %4")
                                     .arg(progress.acknowledged)
                                     .arg(progress.total)
                                     .arg(progress.in_flight)
                                     .arg(progress.queued));
    } else {
        progress_bar_->setValue(0);
        progress_label_->setText(QStringLiteral("—"));
    }

    if (controller_->connected()) {
        start_button_->setEnabled(!controller_->running());
    }
}

bool MachinePanel::confirmSafety(const ProgramBounds& bounds, const std::string& /*program*/) {
    if (skip_safety_->isChecked()) {
        return true;
    }

    const auto& limits = controller_->limits();
    QStringList issues;

    if (limits.travel_known()) {
        if (bounds.min_x < -0.5) {
            issues << QStringLiteral("X 最小值 %1 mm 为负，会走到工件零点左侧")
                          .arg(format_mm(bounds.min_x));
        }
        if (bounds.max_x > limits.max_x + 0.5) {
            issues << QStringLiteral("X 最大值 %1 mm 超过行程上限 %2 mm")
                          .arg(format_mm(bounds.max_x))
                          .arg(format_mm(limits.max_x));
        }
        if (bounds.min_y < -0.5) {
            issues << QStringLiteral("Y 最小值 %1 mm 为负，会走到工件零点上方")
                          .arg(format_mm(bounds.min_y));
        }
        if (bounds.max_y > limits.max_y + 0.5) {
            issues << QStringLiteral("Y 最大值 %1 mm 超过行程上限 %2 mm")
                          .arg(format_mm(bounds.max_y))
                          .arg(format_mm(limits.max_y));
        }
    } else {
        issues << QStringLiteral("尚未读取机器行程（点「读参数 $$」），无法自动检查是否超程");
    }

    if (limits.max_rate_x > 0.0 && bounds.max_feed > limits.max_rate_x + 1.0) {
        issues << QStringLiteral("程序进给 %1 超过机器上限 %2 mm/min，GRBL 会自动截断")
                      .arg(format_mm(bounds.max_feed))
                      .arg(format_mm(limits.max_rate_x));
    }

    const auto& status = controller_->status();
    const QString position =
        status.has_work_position
            ? QStringLiteral("X %1  Y %2  Z %3 mm")
                  .arg(format_mm(status.work[0]))
                  .arg(format_mm(status.work[1]))
                  .arg(format_mm(status.work[2]))
            : QStringLiteral("未知");

    QString text =
        QStringLiteral("即将发送 %1 行程序并开始运动。\n\n"
                       "程序范围：X %2 ~ %3 mm（宽 %4）\n"
                       "          Y %5 ~ %6 mm（高 %7）\n"
                       "最大进给：%8 mm/min\n"
                       "当前工件坐标：%9\n\n"
                       "请确认：\n"
                       "  · 笔已装好、纸已就位\n"
                       "  · 零点已设在纸的左上角（未设置请先点「设零点」）\n"
                       "  · 首次运行建议先取下笔空跑一遍\n\n"
                       "按「确定」开始发送。")
            .arg(bounds.line_count)
            .arg(format_mm(bounds.min_x))
            .arg(format_mm(bounds.max_x))
            .arg(format_mm(bounds.width()))
            .arg(format_mm(bounds.min_y))
            .arg(format_mm(bounds.max_y))
            .arg(format_mm(bounds.height()))
            .arg(format_mm(bounds.max_feed))
            .arg(position);

    if (!issues.isEmpty()) {
        text += QStringLiteral("\n\n⚠ 发现以下问题：\n  · ") +
                issues.join(QStringLiteral("\n  · "));
    }

    QMessageBox box(this);
    box.setWindowTitle(issues.isEmpty() ? QStringLiteral("确认开始书写")
                                        : QStringLiteral("开始前请注意"));
    box.setIcon(issues.isEmpty() ? QMessageBox::Question : QMessageBox::Warning);
    box.setText(text);
    box.setStandardButtons(QMessageBox::Ok | QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Cancel);
    return box.exec() == QMessageBox::Ok;
}

void MachinePanel::onStart() {
    if (!controller_->connected() || program_provider_ == nullptr) {
        return;
    }
    if (controller_->running()) {
        return;
    }
    if (controller_->state() == State::Alarm) {
        QMessageBox::warning(this, QStringLiteral("机器处于告警状态"),
                             QStringLiteral("请先点「解锁 $X」清除告警，再开始书写。"));
        return;
    }

    if (cached_program_.empty() || !cached_bounds_.valid) {
        QMessageBox::warning(this, QStringLiteral("没有可发送的内容"),
                             QStringLiteral("先在左侧输入文字，确保预览里有内容。"));
        return;
    }
    if (!confirmSafety(cached_bounds_, cached_program_)) {
        appendLog(QStringLiteral("已取消发送"));
        return;
    }

    controller_->load(cached_program_);
    controller_->start();
    appendLog(QStringLiteral("开始发送 %1 行").arg(controller_->progress().total));
    updateProgramUi();
}

void MachinePanel::onPause() {
    controller_->pause();
    appendLog(QStringLiteral("已暂停（进给保持）"));
}

void MachinePanel::onResume() {
    controller_->resume();
    appendLog(QStringLiteral("已继续"));
}

void MachinePanel::onAbort() {
    if (!controller_->connected()) {
        return;
    }
    const auto answer = QMessageBox::question(
        this, QStringLiteral("确认中止"),
        QStringLiteral("中止会立即停止进给并软复位。\n"
                       "机床坐标会归零（工件零点不受影响）。\n\n确定中止吗？"));
    if (answer != QMessageBox::Yes) {
        return;
    }
    controller_->abort();
    appendLog(QStringLiteral("已中止并软复位"));
    updateProgramUi();
}

// ---------------------------------------------------------------------------
// 手动控制
// ---------------------------------------------------------------------------

void MachinePanel::onHome() {
    controller_->home();
    appendLog(QStringLiteral("回零 $H"));
}

void MachinePanel::onUnlock() {
    controller_->unlock();
    appendLog(QStringLiteral("解锁 $X"));
}

void MachinePanel::onSetOrigin() {
    controller_->set_work_origin();
    appendLog(QStringLiteral("把当前位置设为工件零点（G10 L20 P1）"));
}

void MachinePanel::onReadSettings() {
    controller_->request_settings();
    appendLog(QStringLiteral("读取机器参数 $$"));
}

void MachinePanel::onPenUp() {
    controller_->pen_up(3.0);
    appendLog(QStringLiteral("抬笔 Z3"));
}

void MachinePanel::onPenDown() {
    controller_->pen_down(0.0);
    appendLog(QStringLiteral("落笔 Z0"));
}

void MachinePanel::jog(double dx, double dy, double dz) {
    const double step = jog_step_->value();
    const double feed = jog_feed_->value();
    controller_->jog(dx * step, dy * step, dz * step, feed);
}

void MachinePanel::appendLog(const QString& text) {
    if (log_view_ == nullptr) {
        return;
    }
    log_view_->appendPlainText(text);
}
