// 串口传输抽象。
//
// 分两层是为了**让协议逻辑能脱离硬件测试**：
//   Transport        纯虚接口，真实串口与测试用假实现都实现它
//   SerialPort       真实串口（Windows 用 Win32 API，Linux/macOS 用 termios）
//   list_ports()     枚举可用串口
//
// 协议层（见 grbl.hpp）只依赖 Transport，因此可以在没有机器的情况下
// 用假实现喂入 GRBL 的各种响应，把状态机、流控、错误处理全部测到。
//
// 为什么不用 QSerialPort：Qt 6.9.3 的 mingw_64 包默认不含 QtSerialPort 模块，
// 装它需要额外下载一整个组件；而 Win32 串口 API 是系统自带的，
// 零依赖，且 POSIX 侧用 termios 同样只有几十行。
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace handwriting::serial {

struct PortInfo {
    std::string name;         // 设备名，如 "COM3"
    std::string description;  // 人类可读描述，如 "USB-SERIAL CH340 (COM3)"
};

// 枚举当前可用的串口。失败时返回空列表（不抛异常）。
[[nodiscard]] std::vector<PortInfo> list_ports();

// 串口传输接口。
class Transport {
public:
    virtual ~Transport() = default;

    [[nodiscard]] virtual bool open(const std::string& port, int baud_rate) = 0;
    virtual void close() = 0;
    [[nodiscard]] virtual bool is_open() const = 0;

    // 写入。返回实际写入的字节数，失败返回 0。
    [[nodiscard]] virtual std::size_t write(const std::string& data) = 0;

    // 读走当前所有可读数据。非阻塞：没有数据时返回空串。
    [[nodiscard]] virtual std::string read() = 0;

    [[nodiscard]] virtual std::string last_error() const = 0;
};

// 真实串口。
class SerialPort final : public Transport {
public:
    SerialPort();
    ~SerialPort() override;

    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;

    [[nodiscard]] bool open(const std::string& port, int baud_rate) override;
    void close() override;
    [[nodiscard]] bool is_open() const override;
    [[nodiscard]] std::size_t write(const std::string& data) override;
    [[nodiscard]] std::string read() override;
    [[nodiscard]] std::string last_error() const override;

    [[nodiscard]] const std::string& port_name() const noexcept { return port_; }
    [[nodiscard]] int baud_rate() const noexcept { return baud_rate_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string port_;
    int baud_rate_{0};
    std::string last_error_;
};

// GRBL 常用的波特率。115200 是默认值，也是最常见的固件配置。
inline constexpr int kDefaultBaudRate = 115200;

inline constexpr int kBaudRates[] = {115200, 250000, 57600, 38400, 19200, 9600};

}  // namespace handwriting::serial
