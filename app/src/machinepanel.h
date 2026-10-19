// 机器控制面板：串口连接、手动控制、程序执行、状态显示。
//
// 与核心库的分工：本面板只做 UI 与调度，协议逻辑全在 handwriting::grbl::Controller。
// 因此「按了什么按钮发什么指令」这类逻辑可以在没有界面的情况下被测试覆盖。
//
// 时间驱动的部分（轮询、状态查询、握手超时）都在这里，核心库不读时钟 ——
// 这样协议层完全可测，不需要靠 sleep 来等待。
#pragma once

#include <QWidget>

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "handwriting/bounds.hpp"
#include "handwriting/grbl.hpp"
#include "handwriting/serial.hpp"

class BoundsWidget;

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTimer;

class MachinePanel : public QWidget {
    Q_OBJECT

public:
    explicit MachinePanel(QWidget* parent = nullptr);
    ~MachinePanel() override;

    /// 由 MainWindow 提供当前要书写的 G 代码。空串表示没有可发的内容。
    void setProgramProvider(std::function<std::string()> provider);

    /// 由 MainWindow 提供抬笔 / 落笔的 Z 值（毫米），供手动「抬笔 / 落笔」按钮使用。
    /// 不同机器的 Z 方向可能相反（有的机器 Z 越大越压得深），所以这里不写死。
    void setPenZProvider(std::function<std::pair<double, double>()> provider);

    /// 程序内容变化时调用，刷新「待发送」提示与安全检查结果。
    void notifyProgramChanged();

    /// 仅供快照/演示使用：注入一个已标定的边界（不写磁盘）。
    void setWorkspace(const handwriting::bounds::Workspace& ws);
    /// 仅供快照/演示使用：注入机头位置（不要求连接）。
    void setDemoHead(double x, double y);

    [[nodiscard]] bool connected() const;

private slots:
    void refreshPorts();
    void toggleConnection();
    void onPoll();
    void onHandshakeTimeout();

    void onStart();
    void onPause();
    void onResume();
    void onAbort();

    void onHome();
    void onUnlock();
    void onSetOrigin();
    void onPenUp();
    void onPenDown();
    void onReadSettings();

    // ---- 工作区标定（软限位） ----
    void onStartCalibrate();
    void onRecordCorner();
    void onCancelCalibrate();
    void onSaveBounds();
    void onClearBounds();
    void onSoftLimitToggled(bool on);

private:
    void buildUi();
    void updateConnectionUi();
    void updateStatusUi();
    void updateProgramUi();
    void appendLog(const QString& text);
    void setEnabledWhenConnected(bool enabled);
    void jog(double dx, double dy, double dz);

    /// 上机前检查：行程、进给、零点、软限位。返回 false 表示用户取消。
    [[nodiscard]] bool confirmSafety(const handwriting::grbl::ProgramBounds& bounds,
                                     const std::string& program);

    void updateBoundsUi();
    void updateCalibrateHint();
    /// 是否正在标定（标定期间点动不受软限位拦截，否则根本到不了边界）。
    [[nodiscard]] bool calibrating() const noexcept { return calibrate_step_ >= 1 && calibrate_step_ <= 4; }

    handwriting::serial::SerialPort transport_;
    std::unique_ptr<handwriting::grbl::Controller> controller_;
    std::function<std::string()> program_provider_;
    std::function<std::pair<double, double>()> pen_z_provider_;

    QTimer* poll_timer_{nullptr};
    QTimer* status_timer_{nullptr};
    QTimer* handshake_timer_{nullptr};

    // 连接
    QComboBox* port_combo_{nullptr};
    QComboBox* baud_combo_{nullptr};
    QPushButton* refresh_button_{nullptr};
    QPushButton* connect_button_{nullptr};
    QLabel* connection_label_{nullptr};

    // 状态
    QLabel* state_label_{nullptr};
    QLabel* position_label_{nullptr};
    QLabel* limits_label_{nullptr};

    // 手动控制
    QPushButton* home_button_{nullptr};
    QPushButton* unlock_button_{nullptr};
    QPushButton* origin_button_{nullptr};
    QPushButton* pen_up_button_{nullptr};
    QPushButton* pen_down_button_{nullptr};
    QPushButton* settings_button_{nullptr};
    QDoubleSpinBox* jog_step_{nullptr};
    QDoubleSpinBox* jog_feed_{nullptr};

    // 程序
    QPushButton* start_button_{nullptr};
    QPushButton* pause_button_{nullptr};
    QPushButton* resume_button_{nullptr};
    QPushButton* abort_button_{nullptr};
    QProgressBar* progress_bar_{nullptr};
    QLabel* progress_label_{nullptr};
    QLabel* program_label_{nullptr};
    QCheckBox* dry_run_{nullptr};
    QCheckBox* skip_safety_{nullptr};

    QPlainTextEdit* log_view_{nullptr};

    // ---- 工作区标定（软限位） ----
    BoundsWidget* bounds_widget_{nullptr};
    QLabel* bounds_label_{nullptr};
    QPushButton* calibrate_button_{nullptr};   ///< 开始标定
    QPushButton* clear_bounds_button_{nullptr}; ///< 清除已标定边界
    QGroupBox* calibrate_wizard_{nullptr};       ///< 标定向导容器（平时隐藏）
    QLabel* calibrate_hint_{nullptr};
    QPushButton* calibrate_record_{nullptr};
    QPushButton* calibrate_cancel_{nullptr};
    QPushButton* calibrate_save_{nullptr};
    QCheckBox* soft_limit_{nullptr};

    int calibrate_step_{0};  ///< 0=未开始/已退出；1..4=记录第 N 个角；5=完成待保存
    std::array<std::pair<double, double>, 4> calibrate_points_{};

    handwriting::bounds::Workspace workspace_;  ///< 已标定（或已加载）的边界

    bool handshake_warned_{false};

    // 程序缓存。绝不能每帧都去问 provider —— 那会跑一遍完整的字形渲染
    // （一页约 40ms），20ms 一次的轮询会把界面彻底卡死。
    // 只在 MainWindow 通知「程序变了」时才重新生成。
    std::string cached_program_;
    handwriting::grbl::ProgramBounds cached_bounds_;

    // 已经倒进日志的错误/消息条数，避免重复刷屏
    std::size_t reported_errors_{0};
    std::size_t reported_messages_{0};
};
