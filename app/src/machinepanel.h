// 机器控制面板：串口连接、手动控制、程序执行、状态显示。
//
// 与核心库的分工：本面板只做 UI 与调度，协议逻辑全在 handwriting::grbl::Controller。
// 因此「按了什么按钮发什么指令」这类逻辑可以在没有界面的情况下被测试覆盖。
//
// 时间驱动的部分（轮询、状态查询、握手超时）都在这里，核心库不读时钟 ——
// 这样协议层完全可测，不需要靠 sleep 来等待。
#pragma once

#include <QWidget>

#include <functional>
#include <memory>
#include <string>

#include "handwriting/grbl.hpp"
#include "handwriting/serial.hpp"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
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

    /// 程序内容变化时调用，刷新「待发送」提示与安全检查结果。
    void notifyProgramChanged();

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

private:
    void buildUi();
    void updateConnectionUi();
    void updateStatusUi();
    void updateProgramUi();
    void appendLog(const QString& text);
    void setEnabledWhenConnected(bool enabled);
    void jog(double dx, double dy, double dz);

    /// 上机前检查：行程、进给、零点。返回 false 表示用户取消。
    [[nodiscard]] bool confirmSafety(const handwriting::grbl::ProgramBounds& bounds,
                                     const std::string& program);

    handwriting::serial::SerialPort transport_;
    std::unique_ptr<handwriting::grbl::Controller> controller_;
    std::function<std::string()> program_provider_;

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
