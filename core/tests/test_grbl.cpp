// GRBL 协议层测试。
//
// 全部使用假传输，不需要真实硬件。这是把协议逻辑与串口 I/O 分开的收益：
// 流控边界、状态解析、告警处理这些最容易出错的地方都能精确验证。
//
// 特别地，TestFlowControl 会逐字节核对「已发送但未确认」的字节数 ——
// 这段逻辑一旦算错，表现是机器随机丢步或卡住，而且极难在现场定位。

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "check.hpp"
#include "handwriting/grbl.hpp"
#include "handwriting/serial.hpp"

using handwriting::grbl::Controller;
using handwriting::grbl::Options;
using handwriting::grbl::ProgramBounds;
using handwriting::grbl::State;
using handwriting::serial::Transport;

namespace {

/// 假串口：把写入的内容攒起来，把预置的内容当作收到的数据。
class FakeTransport : public Transport {
public:
    bool open(const std::string& port, int baud_rate) override {
        if (fail_open) {
            error = "模拟打开失败";
            return false;
        }
        port_ = port;
        baud_ = baud_rate;
        open_ = true;
        return true;
    }

    void close() override { open_ = false; }
    [[nodiscard]] bool is_open() const override { return open_; }

    std::size_t write(const std::string& data) override {
        if (!open_) {
            return 0;
        }
        sent += data;
        return data.size();
    }

    [[nodiscard]] std::string read() override {
        std::string out = incoming;
        incoming.clear();
        return out;
    }

    [[nodiscard]] std::string last_error() const override { return error; }

    // ---- 测试辅助 ----
    std::string take_sent() {
        std::string out = sent;
        sent.clear();
        return out;
    }

    /// 统计已发送内容里的完整行数（实时命令是单字节、不带换行，不计入）
    std::size_t take_sent_lines() {
        const std::string data = take_sent();
        return static_cast<std::size_t>(std::count(data.begin(), data.end(), '\n'));
    }

    /// 取出已发送的每一行（去掉换行）
    std::vector<std::string> take_sent_line_texts() {
        std::vector<std::string> lines;
        std::string current;
        for (const char c : take_sent()) {
            if (c == '\n') {
                if (!current.empty()) {
                    lines.push_back(current);
                }
                current.clear();
            } else if (c != '\r') {
                current += c;
            }
        }
        if (!current.empty()) {
            lines.push_back(current);
        }
        return lines;
    }

    /// 模拟 GRBL：对已收到的每一行回一个 ok
    void ack_all_pending() {
        const std::size_t count = take_sent_lines();
        for (std::size_t i = 0; i < count; ++i) {
            incoming += "ok\r\n";
        }
    }

    void push(const std::string& data) { incoming += data; }

    std::string sent;
    std::string incoming;
    std::string port_;
    int baud_{0};
    bool open_{false};
    bool fail_open{false};
    std::string error;
};

std::string make_program(std::size_t line_count) {
    std::string program;
    for (std::size_t i = 0; i < line_count; ++i) {
        program += "G1 X1 Y1 F1000\n";
    }
    return program;
}

}  // namespace

int main() {
    // ================= ProgramBounds =================
    {
        const std::string program =
            "G21\n"
            "G90\n"
            "G0 X10 Y20\n"
            "G1 X30 Y40 F1200\n"
            "G1 X5 Y50 (注释)\n"
            "M5\n";
        const ProgramBounds bounds = handwriting::grbl::analyze(program);
        HR_CHECK(bounds.valid);
        HR_CHECK_NEAR(bounds.min_x, 5.0);
        HR_CHECK_NEAR(bounds.max_x, 30.0);
        HR_CHECK_NEAR(bounds.min_y, 20.0);
        HR_CHECK_NEAR(bounds.max_y, 50.0);
        HR_CHECK(bounds.motion_count == 3);
        HR_CHECK(bounds.line_count == 6);
        HR_CHECK_NEAR(bounds.width(), 25.0);
        HR_CHECK_NEAR(bounds.height(), 30.0);
    }
    {
        // 分号注释与负数坐标
        const ProgramBounds bounds =
            handwriting::grbl::analyze("G0 X-5 Y-10 ; 起点\nG1 X5 Y10\n");
        HR_CHECK(bounds.valid);
        HR_CHECK_NEAR(bounds.min_x, -5.0);
        HR_CHECK_NEAR(bounds.min_y, -10.0);
        HR_CHECK(bounds.motion_count == 2);
    }
    {
        // 相对坐标 G91
        const ProgramBounds bounds = handwriting::grbl::analyze("G91\nG1 X10 Y0\nG1 X10 Y0\n");
        HR_CHECK(bounds.valid);
        HR_CHECK_NEAR(bounds.max_x, 20.0);
    }
    {
        // 没有运动指令时 valid 为 false
        const ProgramBounds bounds = handwriting::grbl::analyze("G21\nG90\nM5\n");
        HR_CHECK(!bounds.valid);
        HR_CHECK(bounds.line_count == 3);
    }

    // ================= 连接 =================
    {
        FakeTransport transport;
        Controller controller(transport);
        HR_CHECK(controller.connect("COM3", 115200));
        HR_CHECK(controller.connected());
        HR_CHECK(transport.open_);
        HR_CHECK(transport.port_ == "COM3");
        HR_CHECK(transport.baud_ == 115200);
        HR_CHECK(controller.handshake_pending());

        // 连接时会发一个空行探路，让 USB 转串口芯片开始上报数据
        HR_CHECK(transport.take_sent() == "\r\n");
    }
    {
        FakeTransport transport;
        transport.fail_open = true;
        Controller controller(transport);
        HR_CHECK(!controller.connect("COM99", 115200));
        HR_CHECK(!controller.connected());
        HR_CHECK(!controller.last_error().empty());
    }
    {
        // 收到数据即视为握手成功
        FakeTransport transport;
        Controller controller(transport);
        controller.connect("COM3", 115200);
        transport.push("Grbl 1.1f ['$' for help]\r\n");
        controller.poll();
        HR_CHECK(controller.handshake_ok());
        HR_CHECK(!controller.handshake_pending());
        HR_CHECK(controller.messages().size() == 1);
    }
    {
        // 一直没数据 → 调用方判定超时
        FakeTransport transport;
        Controller controller(transport);
        controller.connect("COM3", 115200);
        controller.poll();
        HR_CHECK(controller.handshake_pending());
        controller.mark_handshake_failed();
        HR_CHECK(!controller.handshake_pending());
        HR_CHECK(!controller.last_error().empty());
    }
    {
        // 断开后状态清空
        FakeTransport transport;
        Controller controller(transport);
        controller.connect("COM3", 115200);
        controller.load(make_program(5));
        controller.start();
        controller.disconnect();
        HR_CHECK(!controller.connected());
        HR_CHECK(controller.progress().total == 0);
        HR_CHECK(controller.progress().acknowledged == 0);
    }

    // ================= 流控（核心） =================
    {
        // 每行 "G1 X1 Y1 F1000" = 14 字符 + 换行 = 15 字节
        // 上限 120 字节 => 最多 8 行在途（8×15 = 120）
        FakeTransport transport;
        Options options;
        options.rx_buffer_limit = 120;
        Controller controller(transport, options);
        controller.connect("COM3", 115200);
        transport.take_sent();  // 丢掉探路用的空行

        controller.load(make_program(100));
        HR_CHECK(controller.progress().total == 100);

        controller.start();
        const std::size_t first_batch = transport.take_sent_lines();
        HR_CHECK(first_batch == 8);
        HR_CHECK(controller.progress().in_flight == 8);
        HR_CHECK(controller.progress().queued == 92);

        // 确认一行，应当刚好再放出一行
        transport.push("ok\r\n");
        controller.poll();
        HR_CHECK(transport.take_sent_lines() == 1);
        HR_CHECK(controller.progress().acknowledged == 1);
        HR_CHECK(controller.progress().in_flight == 8);
    }
    {
        // 上限调小，在途行数应随之减少
        FakeTransport transport;
        Options options;
        options.rx_buffer_limit = 44;  // 44 / 15 = 2.9 => 2 行
        Controller controller(transport, options);
        controller.connect("COM3", 115200);
        transport.take_sent();

        controller.load(make_program(50));
        controller.start();
        HR_CHECK(transport.take_sent_lines() == 2);
    }
    {
        // 单行比缓冲上限还长时不能死锁：至少也要放出一行，
        // 否则队列永远不推进，表现为「点了开始没反应」
        FakeTransport transport;
        Options options;
        options.rx_buffer_limit = 8;
        Controller controller(transport, options);
        controller.connect("COM3", 115200);
        transport.take_sent();

        controller.load("G1 X123.456 Y234.567 F1000\nG1 X0 Y0\n");
        controller.start();
        HR_CHECK(transport.take_sent_lines() == 1);
    }
    {
        // 逐行等待模式（排查通信用）
        FakeTransport transport;
        Options options;
        options.wait_for_ok = true;
        Controller controller(transport, options);
        controller.connect("COM3", 115200);
        transport.take_sent();

        controller.load(make_program(10));
        controller.start();
        HR_CHECK(transport.take_sent_lines() == 1);
        transport.push("ok\r\n");
        controller.poll();
        HR_CHECK(transport.take_sent_lines() == 1);
    }

    // ================= 完整流式发送 =================
    {
        FakeTransport transport;
        Controller controller(transport);
        controller.connect("COM3", 115200);
        transport.take_sent();

        controller.load(make_program(20));
        controller.start();
        HR_CHECK(!controller.finished());

        // 反复「GRBL 回 ok → 再泵」直到全部确认
        for (int round = 0; round < 60 && !controller.finished(); ++round) {
            transport.ack_all_pending();
            controller.poll();
        }

        HR_CHECK(controller.finished());
        HR_CHECK(controller.progress().acknowledged == 20);
        HR_CHECK(controller.progress().in_flight == 0);
        HR_CHECK(controller.progress().queued == 0);
        HR_CHECK_NEAR(controller.progress().fraction(), 1.0);
        HR_CHECK(!controller.running());
    }
    {
        // 程序里的注释与空行不应占行数
        FakeTransport transport;
        Controller controller(transport);
        controller.connect("COM3", 115200);
        controller.load("G21\n\n; 注释\nG90 (块注释)\nG1 X1 Y1\n");
        HR_CHECK(controller.progress().total == 3);
    }

    // ================= 状态解析 =================
    {
        FakeTransport transport;
        Controller controller(transport);
        controller.connect("COM3", 115200);
        transport.push("<Idle|MPos:1.500,2.250,3.000|FS:0,0>\r\n");
        controller.poll();

        HR_CHECK(controller.state() == State::Idle);
        HR_CHECK(controller.status().has_machine_position);
        HR_CHECK_NEAR(controller.status().machine[0], 1.5);
        HR_CHECK_NEAR(controller.status().machine[1], 2.25);
        HR_CHECK_NEAR(controller.status().machine[2], 3.0);
        HR_CHECK(controller.status().valid());
    }
    {
        // 状态报告不是换行结尾的，后面紧跟 ok —— 必须能拆开
        FakeTransport transport;
        Controller controller(transport);
        controller.connect("COM3", 115200);
        transport.take_sent();
        controller.load(make_program(3));
        controller.start();

        transport.push("<Run|MPos:10.000,20.000,0.000|FS:1200,0>ok\r\n");
        controller.poll();

        HR_CHECK(controller.state() == State::Run);
        HR_CHECK_NEAR(controller.status().machine[0], 10.0);
        HR_CHECK(controller.progress().acknowledged == 1);
    }
    {
        // 状态报告被拆成两次到达（串口分片是常态）
        FakeTransport transport;
        Controller controller(transport);
        controller.connect("COM3", 115200);
        transport.push("<Hold|MPos:5.0");
        controller.poll();
        HR_CHECK(controller.state() != State::Hold);  // 还不完整，不应误判

        transport.push("00,0.000,0.000|FS:0,0>\r\n");
        controller.poll();
        HR_CHECK(controller.state() == State::Hold);
        HR_CHECK_NEAR(controller.status().machine[0], 5.0);
    }
    {
        // WPos 与进给
        FakeTransport transport;
        Controller controller(transport);
        controller.connect("COM3", 115200);
        transport.push("<Run|WPos:1.000,2.000,0.000|FS:1200,500>\r\n");
        controller.poll();
        HR_CHECK(controller.status().has_work_position);
        HR_CHECK_NEAR(controller.status().work[1], 2.0);
        HR_CHECK(controller.status().feed == 1200);
        HR_CHECK(controller.status().spindle == 500);
    }
    {
        // 各种状态名
        const std::pair<const char*, State> cases[] = {
            {"Idle", State::Idle},   {"Run", State::Run},     {"Hold", State::Hold},
            {"Jog", State::Jog},     {"Alarm", State::Alarm}, {"Door", State::Door},
            {"Check", State::Check}, {"Home", State::Home},   {"Sleep", State::Sleep},
        };
        for (const auto& [text, expected] : cases) {
            HR_CHECK(handwriting::grbl::parse_state(text) == expected);
        }
        HR_CHECK(handwriting::grbl::parse_state("Bogus") == State::Unknown);
    }

    // ================= 错误与告警 =================
    {
        FakeTransport transport;
        Controller controller(transport);
        controller.connect("COM3", 115200);
        transport.take_sent();
        controller.load(make_program(3));
        controller.start();
        const std::size_t in_flight = controller.progress().in_flight;

        transport.push("error:20\r\n");
        controller.poll();

        // GRBL 消费了这一行，流控计数必须跟着扣，否则缓冲会假性占满
        HR_CHECK(controller.progress().in_flight == in_flight - 1);
        HR_CHECK(controller.progress().acknowledged == 1);
        HR_CHECK(!controller.errors().empty());
    }
    {
        FakeTransport transport;
        Controller controller(transport);
        controller.connect("COM3", 115200);
        transport.push("<Alarm|MPos:0,0,0>\r\nALARM:1\r\n");
        controller.poll();

        HR_CHECK(controller.state() == State::Alarm);
        HR_CHECK(!controller.last_error().empty());
        HR_CHECK(controller.last_error().find("$X") != std::string::npos);
    }
    {
        // 无法识别的输出不应污染日志
        FakeTransport transport;
        Controller controller(transport);
        controller.connect("COM3", 115200);
        transport.push("some random noise\r\n\r\n");
        controller.poll();
        HR_CHECK(controller.messages().empty());
        HR_CHECK(controller.errors().empty());
    }

    // ================= 实时命令 =================
    {
        FakeTransport transport;
        Controller controller(transport);
        controller.connect("COM3", 115200);
        transport.take_sent();

        controller.request_status();
        HR_CHECK(transport.take_sent() == "?");

        controller.pause();
        HR_CHECK(transport.take_sent() == "!");

        controller.resume();
        HR_CHECK(transport.take_sent() == "~");
    }
    {
        // 实时命令不应影响流控计数（它们不产生 ok）
        FakeTransport transport;
        Controller controller(transport);
        controller.connect("COM3", 115200);
        transport.take_sent();
        controller.load(make_program(10));
        controller.start();
        const std::size_t in_flight = controller.progress().in_flight;

        transport.take_sent();
        controller.request_status();
        controller.request_status();
        controller.poll();

        HR_CHECK(controller.progress().in_flight == in_flight);
        HR_CHECK(transport.take_sent() == "??");
    }

    // ================= 机器控制 =================
    {
        FakeTransport transport;
        Controller controller(transport);
        controller.connect("COM3", 115200);
        transport.take_sent();

        controller.home();
        controller.unlock();
        const std::vector<std::string> lines = transport.take_sent_line_texts();
        HR_CHECK(lines.size() == 2);
        HR_CHECK(lines[0] == "$H");
        HR_CHECK(lines[1] == "$X");
    }
    {
        FakeTransport transport;
        Controller controller(transport);
        controller.connect("COM3", 115200);
        transport.take_sent();

        controller.jog(10.0, -5.0, 0.0, 1000.0);
        HR_CHECK(transport.take_sent_line_texts()[0] == "$J=G91X10Y-5F1000");

        // 全零位移不应产生指令
        transport.take_sent();
        controller.jog(0.0, 0.0, 0.0, 1000.0);
        HR_CHECK(transport.take_sent().empty());
    }
    {
        FakeTransport transport;
        Controller controller(transport);
        controller.connect("COM3", 115200);
        transport.take_sent();

        controller.set_work_origin();
        HR_CHECK(transport.take_sent_line_texts()[0] == "G10 L20 P1 X0 Y0 Z0");

        controller.set_work_origin(10.0, 20.0, 0.0);
        HR_CHECK(transport.take_sent_line_texts()[0] == "G10 L20 P1 X10 Y20 Z0");

        controller.pen_up(3.0);
        HR_CHECK(transport.take_sent_line_texts()[0] == "G0 Z3");
        controller.pen_down(0.0);
        HR_CHECK(transport.take_sent_line_texts()[0] == "G0 Z0");
    }
    {
        // 手动指令不应计入程序进度
        FakeTransport transport;
        Controller controller(transport);
        controller.connect("COM3", 115200);
        controller.load(make_program(5));
        HR_CHECK(controller.progress().total == 5);

        transport.take_sent();
        controller.home();             // $H 被发出
        transport.ack_all_pending();   // 模拟 GRBL 回 ok
        controller.poll();
        HR_CHECK(controller.progress().acknowledged == 0);
    }
    {
        // 载入程序但还没按「开始」时，手动指令不应把程序带出去。
        // 这是安全性质：否则随手点一下「回零」，机器就毫无预兆地开始写字。
        FakeTransport transport;
        Controller controller(transport);
        controller.connect("COM3", 115200);
        controller.load(make_program(5));
        transport.take_sent();

        controller.home();
        const std::vector<std::string> lines = transport.take_sent_line_texts();
        HR_CHECK(lines.size() == 1);
        HR_CHECK(lines[0] == "$H");

        // 程序行仍在队列里，一行都没发
        HR_CHECK(controller.progress().in_flight == 1);
        HR_CHECK(controller.progress().queued == 5);
        HR_CHECK(controller.progress().acknowledged == 0);
    }
    {
        // 手动指令必须插到程序行之前，否则会被门控挡住永远发不出去
        FakeTransport transport;
        Controller controller(transport);
        controller.connect("COM3", 115200);
        controller.load(make_program(5));
        transport.take_sent();

        controller.home();
        controller.pen_up(3.0);
        const std::vector<std::string> lines = transport.take_sent_line_texts();
        HR_CHECK(lines.size() == 2);
        HR_CHECK(lines[0] == "$H");
        HR_CHECK(lines[1] == "G0 Z3");
    }
    {
        // 程序运行中拒绝手动指令（必须先暂停或中止）
        FakeTransport transport;
        Controller controller(transport);
        controller.connect("COM3", 115200);
        controller.load(make_program(5));
        controller.start();
        transport.take_sent();

        controller.jog(10.0, 0.0, 0.0, 1000.0);
        HR_CHECK(transport.take_sent().empty());
        HR_CHECK(!controller.errors().empty());
        HR_CHECK(controller.last_error().find("暂停") != std::string::npos);

        // 暂停后（running 仍为 true）依然拒绝；中止后才允许
        controller.abort();
        transport.take_sent();
        controller.jog(10.0, 0.0, 0.0, 1000.0);
        HR_CHECK(transport.take_sent().find("$J=G91") != std::string::npos);
    }

    // ================= 中止 =================
    {
        FakeTransport transport;
        Controller controller(transport);
        controller.connect("COM3", 115200);
        controller.load(make_program(50));
        controller.start();
        HR_CHECK(controller.progress().queued > 0);

        transport.take_sent();
        controller.abort();

        const std::string sent = transport.take_sent();
        HR_CHECK(sent.find('!') != std::string::npos);            // 先暂停进给
        HR_CHECK(sent.find('\x18') != std::string::npos);          // 再软复位
        HR_CHECK(controller.progress().queued == 0);
        HR_CHECK(controller.progress().in_flight == 0);
        HR_CHECK(!controller.running());
    }
    {
        // 未连接时所有操作都应是安全的空操作
        FakeTransport transport;
        Controller controller(transport);
        controller.load(make_program(3));
        controller.start();
        controller.pause();
        controller.resume();
        controller.abort();
        controller.request_status();
        controller.jog(1, 1, 0, 100);
        HR_CHECK(!controller.connected());
        HR_CHECK(transport.sent.empty());
    }

    return hrtest::summary("grbl");
}
