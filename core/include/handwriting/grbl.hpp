// GRBL 协议层：串口流式发送、状态解析、机器控制。
//
// 设计要点：**本层不认识串口硬件**，只依赖 serial::Transport 接口。
// 因此可以在没有机器的情况下用假实现喂入 GRBL 的各种响应，
// 把状态机、流控、错误处理全部测到（见 core/tests/test_grbl.cpp）。
//
// ## 流控
//
// GRBL 每接收一行就回一个 ok，但 ok 表示「已进入规划缓冲」，不表示「已走完」。
// 串口接收缓冲只有 128 字节，一次灌太多会溢出丢数据。
// 因此采用标准的**字符计数法**：跟踪「已发送但未确认」的字节数，
// 只要不超上限就继续发，收到 ok 再扣减。
//
// 发送与执行是解耦的：机器实际在走的时间远长于传输时间（见 docs/architecture.md），
// 所以缓冲会一直保持满的状态，传输不会成为瓶颈。
//
// ## 时间
//
// 本层**不读时钟**。状态查询由调用方按自己的节奏触发（Qt 里是定时器），
// 握手超时也由调用方判断。这样协议逻辑完全可测，不依赖真实时间流逝。
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "handwriting/serial.hpp"

namespace handwriting::grbl {

enum class State { Unknown, Idle, Run, Hold, Jog, Alarm, Door, Check, Home, Sleep };

[[nodiscard]] const char* to_string(State state);
[[nodiscard]] State parse_state(const std::string& text);

struct Status {
    State state{State::Unknown};
    bool has_machine_position{false};
    double machine[3]{0.0, 0.0, 0.0};
    bool has_work_position{false};
    double work[3]{0.0, 0.0, 0.0};
    int feed{0};
    int spindle{0};
    std::string raw;
    /// 是否收到过至少一次状态报告
    [[nodiscard]] bool valid() const noexcept { return !raw.empty(); }
};

struct Options {
    /// 串口接收缓冲上限（字节）。GRBL 默认缓冲 128 字节，留出余量。
    std::size_t rx_buffer_limit{120};
    /// 发送每行后是否等待确认（false 才启用字符计数流水线）。
    /// 保留开关用于排查通信问题：逐行等待慢但最容易定位。
    bool wait_for_ok{false};
};

struct Progress {
    std::size_t total{0};          ///< 程序总行数
    std::size_t acknowledged{0};   ///< 已被 GRBL 确认的行数
    std::size_t in_flight{0};      ///< 已发送待确认的行数
    std::size_t queued{0};         ///< 还在本地队列里等待发送的行数

    [[nodiscard]] double fraction() const noexcept {
        return (total == 0) ? 0.0 : static_cast<double>(acknowledged) / static_cast<double>(total);
    }
};

/// 机器参数（来自 GRBL 的 $$ 设置）。用于上机前的安全检查。
struct MachineLimits {
    bool valid{false};
    double max_x{0.0};
    double max_y{0.0};
    double max_z{0.0};
    double max_rate_x{0.0};  ///< mm/min
    double max_rate_y{0.0};
    double max_rate_z{0.0};

    /// 行程是否可用（读到过 $130/$131）
    [[nodiscard]] bool travel_known() const noexcept { return valid && max_x > 0.0 && max_y > 0.0; }
};

/// 程序里的运动范围，用于上机前的安全检查。
struct ProgramBounds {
    bool valid{false};
    double min_x{0.0};
    double min_y{0.0};
    double max_x{0.0};
    double max_y{0.0};
    std::size_t line_count{0};
    std::size_t motion_count{0};
    double max_feed{0.0};  ///< 程序里出现的最大进给速度，用于对照机器上限

    [[nodiscard]] double width() const noexcept { return max_x - min_x; }
    [[nodiscard]] double height() const noexcept { return max_y - min_y; }
};

/// 扫描 G 代码，取出运动范围与行数。
///
/// 上机前用它检查程序是否超出机器行程 —— 直接把超出范围的文件发下去
/// 会撞限位，轻则丢步重则损坏机器。
[[nodiscard]] ProgramBounds analyze(const std::string& program);

class Controller {
public:
    explicit Controller(serial::Transport& transport, Options options = {});

    // ---- 连接 ----
    /// 打开串口。非阻塞：不等待 GRBL 响应，握手结果由 poll() 更新。
    bool connect(const std::string& port, int baud_rate = serial::kDefaultBaudRate);
    void disconnect();
    [[nodiscard]] bool connected() const noexcept { return connected_; }
    /// 握手状态：等待中 / 已收到 GRBL 数据 / 超时（由调用方判定）
    [[nodiscard]] bool handshake_pending() const noexcept { return handshake_pending_; }
    [[nodiscard]] bool handshake_ok() const noexcept { return handshake_ok_; }
    /// 调用方判定超时后调用，把状态标记为失败
    void mark_handshake_failed();

    // ---- 主循环 ----
    /// 非阻塞泵：读串口、解析响应、推进发送队列。需要被高频调用（约 20ms 一次）。
    void poll();

    // ---- 程序控制 ----
    void load(std::string program);
    void start();
    void pause();
    void resume();
    /// 中止：进给保持 + 清空本地队列 + 软复位
    void abort();
    [[nodiscard]] bool running() const noexcept { return running_; }

    // ---- 机器控制 ----
    void home();                                  ///< $H 回零
    void unlock();                                ///< $X 清除告警
    void jog(double dx, double dy, double dz, double feed);
    void set_work_origin();                       ///< 把当前位置设为零点
    void set_work_origin(double x, double y, double z);
    void pen_up(double z);
    void pen_down(double z);
    void request_status();                        ///< 实时查询，随时可发
    void request_settings();                      ///< 发 $$ 读取机器参数

    // ---- 查询 ----
    [[nodiscard]] const Status& status() const noexcept { return status_; }
    [[nodiscard]] const Progress& progress() const noexcept { return progress_; }
    [[nodiscard]] State state() const noexcept { return status_.state; }
    [[nodiscard]] const MachineLimits& limits() const noexcept { return limits_; }
    [[nodiscard]] bool finished() const noexcept;
    [[nodiscard]] const std::vector<std::string>& messages() const noexcept { return messages_; }
    [[nodiscard]] const std::vector<std::string>& errors() const noexcept { return errors_; }
    [[nodiscard]] std::string last_error() const;
    void clear_log();

    /// 最近一次通信错误（串口层），空串表示正常
    [[nodiscard]] std::string transport_error() const;

private:
    struct PendingLine {
        std::string text;
        std::size_t bytes{0};  ///< 含换行符
        /// 是否属于要书写的程序。程序行只在「运行中」才发送，
        /// 且只有它们计入进度；手动指令（回零、点动、设零点…）两者都不占。
        bool is_program{false};
    };

    void consume_buffer();
    void handle_line(const std::string& line);
    void handle_status_report(const std::string& body);
    void handle_settings_line(const std::string& line);
    void pump_queue();
    void enqueue(std::string line, bool is_program);
    /// 手动指令的前置检查：未连接或正在运行程序时拒绝执行
    [[nodiscard]] bool allow_manual(const char* action);
    void write_now(const std::string& data);
    void acknowledge();
    void note_error(std::string text);
    void note_message(std::string text);

    serial::Transport& transport_;
    Options options_;

    std::string buffer_;
    std::deque<PendingLine> queue_;
    std::deque<PendingLine> in_flight_;
    std::size_t in_flight_bytes_{0};

    Status status_;
    Progress progress_;
    MachineLimits limits_;
    std::vector<std::string> messages_;
    std::vector<std::string> errors_;

    bool connected_{false};
    bool handshake_pending_{false};
    bool handshake_ok_{false};
    bool running_{false};
    bool aborted_{false};
};

}  // namespace handwriting::grbl
