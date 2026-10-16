// 串口实现（Linux / macOS，termios）。
//
// 本机是 Windows，这份实现没有在真实硬件上验证过 —— 它存在的意义是
// 让核心库能在 Linux CI 上构建，并为将来在 Linux 上跑写字机留好接口。
// 协议层（grbl.hpp）与平台无关，已由假传输的单元测试完整覆盖。
#include "handwriting/serial.hpp"

#ifndef _WIN32

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <system_error>

#ifdef __linux__
#include <asm/ioctl.h>
#include <asm/termbits.h>
#endif

namespace handwriting::serial {

namespace {

std::string describe_errno(int code) {
    switch (code) {
        case ENOENT:
            return "串口不存在";
        case EACCES:
        case EPERM:
            return "串口被占用或权限不足（Linux 下需加入 dialout 组）";
        case EBUSY:
            return "串口已被其他程序打开";
        case EINVAL:
            return "串口参数无效";
        default:
            return std::strerror(code);
    }
}

// 标准波特率表。250000 这类非标准值不在其中，需走 Linux 专有的 BOTHER。
struct BaudEntry {
    int rate;
    speed_t constant;
};

constexpr BaudEntry kBaudTable[] = {
    {9600, B9600},     {19200, B19200},   {38400, B38400},   {57600, B57600},
    {115200, B115200}, {230400, B230400}, {460800, B460800}, {921600, B921600},
};

bool lookup_baud(int baud_rate, speed_t& out) {
    for (const BaudEntry& entry : kBaudTable) {
        if (entry.rate == baud_rate) {
            out = entry.constant;
            return true;
        }
    }
    return false;
}

}  // namespace

std::vector<PortInfo> list_ports() {
    std::vector<PortInfo> ports;
    std::error_code error;

    const auto scan = [&ports, &error](const std::filesystem::path& directory,
                                       const std::string& prefix) {
        if (!std::filesystem::is_directory(directory, error)) {
            return;
        }
        for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
            const std::string name = entry.path().filename().string();
            if (name.rfind(prefix, 0) == 0) {
                ports.push_back(PortInfo{entry.path().string(), entry.path().string()});
            }
        }
    };

    scan("/dev", "ttyUSB");
    scan("/dev", "ttyACM");
    scan("/dev", "ttyS");
    scan("/dev", "cu.");   // macOS
    scan("/dev", "tty.");  // macOS

    std::sort(ports.begin(), ports.end(),
              [](const PortInfo& a, const PortInfo& b) { return a.name < b.name; });
    return ports;
}

struct SerialPort::Impl {
    int fd{-1};
};

SerialPort::SerialPort() : impl_(std::make_unique<Impl>()) {}

SerialPort::~SerialPort() {
    close();
}

bool SerialPort::open(const std::string& port, int baud_rate) {
    close();
    last_error_.clear();

    const int fd = ::open(port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        last_error_ = describe_errno(errno);
        return false;
    }

    termios config{};
    if (tcgetattr(fd, &config) != 0) {
        last_error_ = std::string("无法读取串口配置：") + std::strerror(errno);
        ::close(fd);
        return false;
    }

    cfmakeraw(&config);  // 关闭回显、行缓冲、信号处理 —— 串口通信必须原始模式
    config.c_cflag |= CLOCAL | CREAD;
    config.c_cflag &= ~CRTSCTS;  // GRBL 不用硬件流控
    config.c_cc[VMIN] = 0;       // 非阻塞读
    config.c_cc[VTIME] = 0;

    speed_t speed = B115200;
    const bool found = lookup_baud(baud_rate, speed);

    if (found) {
        cfsetispeed(&config, speed);
        cfsetospeed(&config, speed);
    }
#ifdef __linux__
    else {
        // 250000 这类非标准波特率在很多 GRBL 板子上是默认值，
        // termios 的标准常量里没有，必须走 Linux 专有的 BOTHER。
        struct termios2 raw{};
        if (ioctl(fd, TCGETS2, &raw) != 0) {
            last_error_ = std::string("无法读取串口配置：") + std::strerror(errno);
            ::close(fd);
            return false;
        }
        raw.c_cflag &= ~CBAUD;
        raw.c_cflag |= BOTHER;
        raw.c_ispeed = static_cast<speed_t>(baud_rate);
        raw.c_ospeed = static_cast<speed_t>(baud_rate);
        if (ioctl(fd, TCSETS2, &raw) != 0) {
            last_error_ = "不支持的波特率 " + std::to_string(baud_rate);
            ::close(fd);
            return false;
        }
        // 已经用 termios2 设好了，跳过下面的 tcsetattr
        impl_->fd = fd;
        port_ = port;
        baud_rate_ = baud_rate;
        return true;
    }
#else
    else {
        last_error_ = "本平台不支持波特率 " + std::to_string(baud_rate);
        ::close(fd);
        return false;
    }
#endif

    tcflush(fd, TCIOFLUSH);
    if (tcsetattr(fd, TCSANOW, &config) != 0) {
        last_error_ = std::string("无法设置串口参数：") + std::strerror(errno);
        ::close(fd);
        return false;
    }

    impl_->fd = fd;
    port_ = port;
    baud_rate_ = baud_rate;
    return true;
}

void SerialPort::close() {
    if (impl_ && impl_->fd >= 0) {
        ::close(impl_->fd);
        impl_->fd = -1;
    }
    port_.clear();
    baud_rate_ = 0;
}

bool SerialPort::is_open() const {
    return impl_ && impl_->fd >= 0;
}

std::size_t SerialPort::write(const std::string& data) {
    if (!is_open() || data.empty()) {
        return 0;
    }
    const ssize_t written = ::write(impl_->fd, data.data(), data.size());
    if (written < 0) {
        last_error_ = std::string("写入失败：") + std::strerror(errno);
        return 0;
    }
    return static_cast<std::size_t>(written);
}

std::string SerialPort::read() {
    if (!is_open()) {
        return {};
    }
    std::string out;
    char buffer[1024];
    while (true) {
        const ssize_t got = ::read(impl_->fd, buffer, sizeof(buffer));
        if (got <= 0) {
            break;
        }
        out.append(buffer, static_cast<std::size_t>(got));
        if (got < static_cast<ssize_t>(sizeof(buffer))) {
            break;
        }
    }
    return out;
}

std::string SerialPort::last_error() const {
    return last_error_;
}

}  // namespace handwriting::serial

#endif  // !_WIN32
