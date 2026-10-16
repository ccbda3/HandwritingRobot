#include "handwriting/grbl.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace handwriting::grbl {

namespace {

constexpr char kSoftReset = '\x18';
constexpr char kFeedHold = '!';
constexpr char kCycleStart = '~';
constexpr char kStatusQuery = '?';

void trim(std::string& text) {
    const auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
    text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
}

/// 去掉行尾注释：`;` 之后、以及成对的 `(...)`
void strip_comment(std::string& line) {
    const std::size_t semicolon = line.find(';');
    if (semicolon != std::string::npos) {
        line.erase(semicolon);
    }
    while (true) {
        const std::size_t open = line.find('(');
        if (open == std::string::npos) {
            break;
        }
        const std::size_t close = line.find(')', open);
        if (close == std::string::npos) {
            line.erase(open);
            break;
        }
        line.erase(open, close - open + 1);
    }
    trim(line);
}

struct Word {
    char letter{0};
    double value{0.0};
};

/// 把 G 代码行切成「字母 + 数值」的词。G0 X10.5 Y-3 => {G,0} {X,10.5} {Y,-3}
std::vector<Word> tokenize(const std::string& line) {
    std::vector<Word> words;
    std::size_t i = 0;
    while (i < line.size()) {
        const unsigned char c = static_cast<unsigned char>(line[i]);
        if (std::isspace(c) != 0) {
            ++i;
            continue;
        }
        if (std::isalpha(c) == 0) {
            ++i;
            continue;
        }
        const char letter = static_cast<char>(std::toupper(c));
        ++i;
        const char* start = line.c_str() + i;
        char* end = nullptr;
        const double value = std::strtod(start, &end);
        if (end != start) {
            words.push_back(Word{letter, value});
            i += static_cast<std::size_t>(end - start);
        }
    }
    return words;
}

std::string format_number(double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.3f", value);
    std::string text(buffer);
    if (text.find('.') != std::string::npos) {
        while (text.size() > 1 && text.back() == '0') {
            text.pop_back();
        }
        if (!text.empty() && text.back() == '.') {
            text.pop_back();
        }
    }
    return text == "-0" ? "0" : text;
}

bool is_motion_code(double value) {
    // G0 / G1 / G2 / G3（含 G00 / G01 等写法）
    return std::fabs(value) < 0.5 || std::fabs(value - 1.0) < 0.5 ||
           std::fabs(value - 2.0) < 0.5 || std::fabs(value - 3.0) < 0.5;
}

}  // namespace

const char* to_string(State state) {
    switch (state) {
        case State::Idle:
            return "空闲";
        case State::Run:
            return "运行中";
        case State::Hold:
            return "已暂停";
        case State::Jog:
            return "点动";
        case State::Alarm:
            return "告警";
        case State::Door:
            return "安全门打开";
        case State::Check:
            return "校验模式";
        case State::Home:
            return "回零中";
        case State::Sleep:
            return "休眠";
        case State::Unknown:
            break;
    }
    return "未知";
}

State parse_state(const std::string& text) {
    if (text == "Idle") return State::Idle;
    if (text == "Run") return State::Run;
    if (text == "Hold") return State::Hold;
    if (text == "Jog") return State::Jog;
    if (text == "Alarm") return State::Alarm;
    if (text == "Door") return State::Door;
    if (text == "Check") return State::Check;
    if (text == "Home") return State::Home;
    if (text == "Sleep") return State::Sleep;
    return State::Unknown;
}

ProgramBounds analyze(const std::string& program) {
    ProgramBounds bounds;
    bool has_xy = false;
    double x = 0.0;
    double y = 0.0;
    bool absolute = true;

    std::istringstream stream(program);
    std::string raw;
    while (std::getline(stream, raw)) {
        std::string line = raw;
        strip_comment(line);
        if (line.empty()) {
            continue;
        }
        ++bounds.line_count;

        const std::vector<Word> words = tokenize(line);
        bool motion = false;
        for (const Word& word : words) {
            if (word.letter == 'G' && is_motion_code(word.value)) {
                motion = true;
            } else if (word.letter == 'G' && std::fabs(word.value - 90.0) < 0.5) {
                absolute = true;
            } else if (word.letter == 'G' && std::fabs(word.value - 91.0) < 0.5) {
                absolute = false;
            } else if (word.letter == 'F') {
                bounds.max_feed = std::max(bounds.max_feed, word.value);
            } else if (word.letter == 'X') {
                x = absolute ? word.value : x + word.value;
            } else if (word.letter == 'Y') {
                y = absolute ? word.value : y + word.value;
            }
        }

        if (!motion) {
            continue;
        }
        ++bounds.motion_count;

        if (!has_xy) {
            bounds.min_x = bounds.max_x = x;
            bounds.min_y = bounds.max_y = y;
            has_xy = true;
        } else {
            bounds.min_x = std::min(bounds.min_x, x);
            bounds.max_x = std::max(bounds.max_x, x);
            bounds.min_y = std::min(bounds.min_y, y);
            bounds.max_y = std::max(bounds.max_y, y);
        }
    }

    bounds.valid = has_xy;
    return bounds;
}

// ---------------------------------------------------------------------------
// Controller
// ---------------------------------------------------------------------------

Controller::Controller(serial::Transport& transport, Options options)
    : transport_(transport), options_(options) {}

bool Controller::connect(const std::string& port, int baud_rate) {
    disconnect();

    if (!transport_.open(port, baud_rate)) {
        note_error("打开串口失败：" + transport_.last_error());
        return false;
    }

    connected_ = true;
    handshake_pending_ = true;
    handshake_ok_ = false;

    // 发一个空行探路。有些 USB 转串口芯片在主机首次写入前不会开始上报数据，
    // 只靠读是等不到 GRBL 的欢迎信息的。
    write_now("\r\n");
    return true;
}

void Controller::disconnect() {
    if (connected_) {
        transport_.close();
    }
    connected_ = false;
    running_ = false;
    aborted_ = false;
    handshake_pending_ = false;
    handshake_ok_ = false;
    buffer_.clear();
    queue_.clear();
    in_flight_.clear();
    in_flight_bytes_ = 0;
    status_ = Status{};
    progress_ = Progress{};
}

void Controller::mark_handshake_failed() {
    if (handshake_pending_) {
        handshake_pending_ = false;
        note_error("串口已打开但没有收到 GRBL 响应。请确认：波特率是否为 115200、"
                   "串口是否被其他程序占用、板子是否已上电。");
    }
}

void Controller::poll() {
    if (!connected_) {
        return;
    }

    const std::string data = transport_.read();
    if (!data.empty()) {
        handshake_ok_ = true;
        handshake_pending_ = false;
        buffer_ += data;
        consume_buffer();
    }
    pump_queue();
}

void Controller::consume_buffer() {
    while (true) {
        const std::size_t status_open = buffer_.find('<');
        const std::size_t newline = buffer_.find('\n');

        // 状态报告不是换行结尾的（GRBL 以 '>' 收尾），必须单独识别，
        // 否则它会和后面的 ok 粘成一行，两边都解析不了。
        if (status_open != std::string::npos &&
            (newline == std::string::npos || status_open < newline)) {
            const std::size_t status_close = buffer_.find('>', status_open);
            if (status_close == std::string::npos) {
                break;  // 还没收全，等下一批数据
            }
            handle_status_report(buffer_.substr(status_open + 1, status_close - status_open - 1));
            buffer_.erase(0, status_close + 1);
            continue;
        }

        if (newline != std::string::npos) {
            std::string line = buffer_.substr(0, newline);
            buffer_.erase(0, newline + 1);
            trim(line);
            if (!line.empty()) {
                handle_line(line);
            }
            continue;
        }

        // 缓冲里剩下的是不完整的一行，等下一批数据
        // 防止异常情况下无限增长
        if (buffer_.size() > 4096) {
            buffer_.erase(0, buffer_.size() - 512);
        }
        break;
    }
}

void Controller::handle_line(const std::string& line) {
    if (line == "ok") {
        acknowledge();
        return;
    }
    if (line.rfind("error:", 0) == 0) {
        note_error("GRBL " + line + "（该行被丢弃，检查 G 代码是否合法）");
        acknowledge();  // GRBL 消费了这一行，流控计数要跟着扣
        return;
    }
    if (line.rfind("ALARM:", 0) == 0) {
        status_.state = State::Alarm;
        running_ = false;
        note_error("GRBL " + line + "（机器进入告警，需先点「解锁」发送 $X）");
        return;
    }
    if (line.rfind("Grbl ", 0) == 0) {
        note_message(line);
        return;
    }
    if (line.rfind("[MSG:", 0) == 0) {
        note_message(line);
        return;
    }
    if (line.rfind("[GC:", 0) == 0) {
        return;  // 解析器状态，暂时用不上
    }
    if (!line.empty() && line[0] == '$') {
        handle_settings_line(line);
        note_message(line);
        return;
    }
    // 其它无法识别的输出直接忽略，避免噪声刷屏
}

void Controller::handle_status_report(const std::string& body) {
    status_.raw = body;

    std::size_t start = 0;
    bool first_field = true;
    while (start <= body.size()) {
        const std::size_t bar = body.find('|', start);
        const std::size_t end = (bar == std::string::npos) ? body.size() : bar;
        const std::string field = body.substr(start, end - start);

        if (first_field) {
            status_.state = parse_state(field);
            first_field = false;
        } else if (field.rfind("MPos:", 0) == 0) {
            const int count = std::sscanf(field.c_str() + 5, "%lf,%lf,%lf", &status_.machine[0],
                                          &status_.machine[1], &status_.machine[2]);
            status_.has_machine_position = count == 3;
        } else if (field.rfind("WPos:", 0) == 0) {
            const int count = std::sscanf(field.c_str() + 5, "%lf,%lf,%lf", &status_.work[0],
                                          &status_.work[1], &status_.work[2]);
            status_.has_work_position = count == 3;
        } else if (field.rfind("FS:", 0) == 0) {
            std::sscanf(field.c_str() + 3, "%d,%d", &status_.feed, &status_.spindle);
        }

        if (bar == std::string::npos) {
            break;
        }
        start = bar + 1;
    }
}

void Controller::handle_settings_line(const std::string& line) {
    // 形如 "$130=300.000"
    const std::size_t equals = line.find('=');
    if (equals == std::string::npos || equals < 2) {
        return;
    }
    const int number = std::atoi(line.substr(1, equals - 1).c_str());
    const double value = std::strtod(line.c_str() + equals + 1, nullptr);

    switch (number) {
        case 130: limits_.max_x = value; break;
        case 131: limits_.max_y = value; break;
        case 132: limits_.max_z = value; break;
        case 110: limits_.max_rate_x = value; break;
        case 111: limits_.max_rate_y = value; break;
        case 112: limits_.max_rate_z = value; break;
        default:
            return;  // 其它设置项暂时用不上
    }
    limits_.valid = true;
}

void Controller::pump_queue() {
    if (!connected_) {
        return;
    }

    while (!queue_.empty()) {
        const PendingLine next = queue_.front();  // 先拷贝，pop 后引用会失效

        // 程序行只在「运行中」才发送。否则载入程序后随手点一下「回零」，
        // 整个程序就会跟着被发下去 —— 机器毫无预兆地开始动，非常危险。
        if (next.is_program && !running_) {
            break;
        }
        if (options_.wait_for_ok && !in_flight_.empty()) {
            break;
        }
        // 缓冲满了就停手。但**在途为空时无论如何都要放出一行**：
        // 否则遇到单行比缓冲上限还长的情况（比如坐标特别长）就会死锁，
        // 表现为「点了开始没反应」，而且完全看不出原因。
        if (in_flight_bytes_ + next.bytes > options_.rx_buffer_limit && !in_flight_.empty()) {
            break;
        }
        write_now(next.text + "\n");
        in_flight_bytes_ += next.bytes;
        in_flight_.push_back(next);
        queue_.pop_front();
    }

    progress_.queued = queue_.size();
    progress_.in_flight = in_flight_.size();
}

void Controller::enqueue(std::string line, bool is_program) {
    trim(line);
    if (line.empty()) {
        return;
    }
    PendingLine pending;
    pending.text = std::move(line);
    pending.bytes = pending.text.size() + 1;  // +1 是换行符
    pending.is_program = is_program;

    if (is_program) {
        queue_.push_back(std::move(pending));
        return;
    }

    // 手动指令插到所有程序行**之前**。
    // 程序行会被「未运行不发送」的门控挡住，若把手动指令排在它们后面，
    // 手动指令就永远发不出去 —— 表现为「点了回零没反应」。
    const auto first_program =
        std::find_if(queue_.begin(), queue_.end(),
                     [](const PendingLine& item) { return item.is_program; });
    queue_.insert(first_program, std::move(pending));
}

bool Controller::allow_manual(const char* action) {
    if (!connected_) {
        return false;
    }
    if (running_) {
        note_error(std::string(action) + " 需要先暂停或中止当前程序");
        return false;
    }
    return true;
}

void Controller::write_now(const std::string& data) {
    if (!connected_) {
        return;
    }
    if (transport_.write(data) != data.size()) {
        note_error("串口写入失败：" + transport_.last_error());
    }
}

void Controller::acknowledge() {
    if (in_flight_.empty()) {
        return;
    }
    const PendingLine line = in_flight_.front();
    in_flight_.pop_front();
    in_flight_bytes_ -= std::min(in_flight_bytes_, line.bytes);

    if (line.is_program) {
        ++progress_.acknowledged;
    }

    progress_.queued = queue_.size();
    progress_.in_flight = in_flight_.size();

    if (running_ && finished()) {
        running_ = false;
    }
}

void Controller::note_error(std::string text) {
    errors_.push_back(std::move(text));
    if (errors_.size() > 200) {
        errors_.erase(errors_.begin());
    }
}

void Controller::note_message(std::string text) {
    messages_.push_back(std::move(text));
    if (messages_.size() > 200) {
        messages_.erase(messages_.begin());
    }
}

void Controller::clear_log() {
    messages_.clear();
    errors_.clear();
}

std::string Controller::last_error() const {
    return errors_.empty() ? std::string{} : errors_.back();
}

std::string Controller::transport_error() const {
    return transport_.last_error();
}

bool Controller::finished() const noexcept {
    return progress_.total > 0 && progress_.acknowledged >= progress_.total &&
           queue_.empty() && in_flight_.empty();
}

void Controller::load(std::string program) {
    queue_.clear();
    in_flight_.clear();
    in_flight_bytes_ = 0;
    progress_ = Progress{};
    aborted_ = false;

    std::istringstream stream(program);
    std::string raw;
    while (std::getline(stream, raw)) {
        std::string line = raw;
        strip_comment(line);
        if (line.empty()) {
            continue;
        }
        enqueue(line, true);
    }

    progress_.total = queue_.size();
    progress_.queued = queue_.size();
}

void Controller::start() {
    if (!connected_ || progress_.total == 0) {
        return;
    }
    running_ = true;
    // 若机器停在暂停状态，先恢复进给
    if (status_.state == State::Hold) {
        resume();
    }
    pump_queue();
}

void Controller::pause() {
    if (!connected_) {
        return;
    }
    write_now(std::string(1, kFeedHold));  // 实时命令，不进队列
}

void Controller::resume() {
    if (!connected_) {
        return;
    }
    write_now(std::string(1, kCycleStart));
}

void Controller::abort() {
    if (!connected_) {
        return;
    }
    write_now(std::string(1, kFeedHold));
    queue_.clear();
    in_flight_.clear();
    in_flight_bytes_ = 0;
    running_ = false;
    aborted_ = true;

    // 软复位。注意：GRBL 软复位后会丢失机床坐标（位置读数归零），
    // 用 G10 L20 设过的工件零点存在 EEPROM 里，不受影响。
    write_now(std::string(1, kSoftReset));

    progress_.queued = 0;
    progress_.in_flight = 0;
    note_message("已中止并软复位。机床坐标已归零，工件零点不受影响。");
}

void Controller::home() {
    if (!allow_manual("回零")) {
        return;
    }
    enqueue("$H", false);
    pump_queue();
}

void Controller::unlock() {
    if (!allow_manual("解锁")) {
        return;
    }
    enqueue("$X", false);
    pump_queue();
}

void Controller::jog(double dx, double dy, double dz, double feed) {
    if (!allow_manual("点动")) {
        return;
    }
    // $J=G91 是 GRBL 1.1 的连续点动命令。用相对坐标，避免误改工件坐标系。
    std::string command = "$J=G91";
    if (std::fabs(dx) > 1e-9) {
        command += "X" + format_number(dx);
    }
    if (std::fabs(dy) > 1e-9) {
        command += "Y" + format_number(dy);
    }
    if (std::fabs(dz) > 1e-9) {
        command += "Z" + format_number(dz);
    }
    if (std::fabs(dx) < 1e-9 && std::fabs(dy) < 1e-9 && std::fabs(dz) < 1e-9) {
        return;
    }
    command += "F" + format_number(feed);
    enqueue(std::move(command), false);
    pump_queue();
}

void Controller::set_work_origin() {
    set_work_origin(0.0, 0.0, 0.0);
}

void Controller::set_work_origin(double x, double y, double z) {
    if (!allow_manual("设置零点")) {
        return;
    }
    // G10 L20 P1 把 G54 工件坐标系设成「当前机床位置 = 给定值」，
    // 并写入 EEPROM，断电不丢。比 G92 更稳妥（G92 是临时偏移，易被后续程序清掉）。
    std::string command = "G10 L20 P1 X" + format_number(x) + " Y" + format_number(y) + " Z" +
                          format_number(z);
    enqueue(std::move(command), false);
    pump_queue();
}

void Controller::pen_up(double z) {
    if (!allow_manual("抬笔")) {
        return;
    }
    enqueue("G0 Z" + format_number(z), false);
    pump_queue();
}

void Controller::pen_down(double z) {
    if (!allow_manual("落笔")) {
        return;
    }
    enqueue("G0 Z" + format_number(z), false);
    pump_queue();
}

void Controller::request_settings() {
    if (!connected_) {
        return;
    }
    enqueue("$$", false);
    pump_queue();
}

void Controller::request_status() {
    if (!connected_) {
        return;
    }
    // ? 是实时命令，GRBL 会立刻回一个状态报告，不受缓冲满影响
    write_now(std::string(1, kStatusQuery));
}

}  // namespace handwriting::grbl
