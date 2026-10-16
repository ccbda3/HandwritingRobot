// 串口实现（Windows / Win32 API）。
#include "handwriting/serial.hpp"

#ifdef _WIN32

#include <windows.h>

#include <devguid.h>
#include <setupapi.h>

#include <algorithm>
#include <array>
#include <cstdio>

namespace handwriting::serial {

namespace {

std::string wide_to_utf8(const wchar_t* text) {
    if (text == nullptr) {
        return {};
    }
    const int needed =
        WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

// 从 "USB-SERIAL CH340 (COM3)" 里取出 "COM3"。
// 取不到就返回空串，调用方会退化为用设备名本身。
std::string extract_port_name(const std::string& description) {
    const std::size_t open = description.rfind('(');
    const std::size_t close = description.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close <= open + 1) {
        return {};
    }
    return description.substr(open + 1, close - open - 1);
}

std::string describe_error(DWORD code) {
    switch (code) {
        case ERROR_FILE_NOT_FOUND:
            return "串口不存在";
        case ERROR_ACCESS_DENIED:
            return "串口被占用（可能已被其他程序打开）";
        case ERROR_BAD_COMMAND:
            return "串口无法访问";
        case ERROR_OPERATION_ABORTED:
            return "操作被取消";
        case ERROR_INVALID_PARAMETER:
            return "串口参数无效";
        default:
            return "系统错误 " + std::to_string(code);
    }
}

}  // namespace

std::vector<PortInfo> list_ports() {
    std::vector<PortInfo> ports;

    // 用 SetupAPI 而不是读注册表：注册表只有 "COM3"，而 SetupAPI 能拿到
    // "USB-SERIAL CH340 (COM3)" 这类友好名。用户接的是哪块板子，
    // 靠友好名一眼就能认出来，靠 COM 号只能靠猜。
    HDEVINFO devices = SetupDiGetClassDevsW(&GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);
    if (devices == INVALID_HANDLE_VALUE) {
        return ports;
    }

    SP_DEVINFO_DATA data{};
    data.cbSize = sizeof(data);

    for (DWORD index = 0; SetupDiEnumDeviceInfo(devices, index, &data); ++index) {
        std::array<wchar_t, 512> buffer{};
        if (SetupDiGetDeviceRegistryPropertyW(devices, &data, SPDRP_FRIENDLYNAME, nullptr,
                                              reinterpret_cast<PBYTE>(buffer.data()),
                                              static_cast<DWORD>(buffer.size() * sizeof(wchar_t)),
                                              nullptr) == FALSE) {
            continue;
        }

        PortInfo info;
        info.description = wide_to_utf8(buffer.data());
        info.name = extract_port_name(info.description);
        if (info.name.empty()) {
            // 兼容没有友好名的老设备：直接去读 PortName
            std::array<wchar_t, 64> port_name{};
            HKEY key = SetupDiOpenDevRegKey(devices, &data, DICS_FLAG_GLOBAL, 0, DIREG_DEV,
                                            KEY_READ);
            if (key != INVALID_HANDLE_VALUE) {
                DWORD size = static_cast<DWORD>(port_name.size() * sizeof(wchar_t));
                DWORD type = 0;
                if (RegQueryValueExW(key, L"PortName", nullptr, &type,
                                     reinterpret_cast<LPBYTE>(port_name.data()),
                                     &size) == ERROR_SUCCESS) {
                    info.name = wide_to_utf8(port_name.data());
                }
                RegCloseKey(key);
            }
        }
        if (!info.name.empty()) {
            if (info.description.empty()) {
                info.description = info.name;
            }
            ports.push_back(std::move(info));
        }
    }

    SetupDiDestroyDeviceInfoList(devices);

    std::sort(ports.begin(), ports.end(),
              [](const PortInfo& a, const PortInfo& b) { return a.name < b.name; });
    return ports;
}

struct SerialPort::Impl {
    HANDLE handle{INVALID_HANDLE_VALUE};
};

SerialPort::SerialPort() : impl_(std::make_unique<Impl>()) {}

SerialPort::~SerialPort() {
    close();
}

bool SerialPort::open(const std::string& port, int baud_rate) {
    close();
    last_error_.clear();

    // COM10 及以上必须写成 \\.\COM10，否则 CreateFile 会失败
    const std::string device = "\\\\.\\" + port;

    HANDLE handle = CreateFileA(device.c_str(), GENERIC_READ | GENERIC_WRITE,
                                0,  // 独占：串口不能被两个程序同时打开
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        last_error_ = describe_error(GetLastError());
        return false;
    }

    DCB config{};
    config.DCBlength = sizeof(config);
    if (GetCommState(handle, &config) == FALSE) {
        last_error_ = "无法读取串口配置：" + describe_error(GetLastError());
        CloseHandle(handle);
        return false;
    }

    config.BaudRate = static_cast<DWORD>(baud_rate);
    config.ByteSize = 8;
    config.Parity = NOPARITY;
    config.StopBits = ONESTOPBIT;
    config.fBinary = TRUE;
    config.fParity = FALSE;
    // GRBL 不需要硬件流控。DTR/RTS 保持有效，某些 USB 转串口芯片靠它判断主机在线，
    // 拉低会让板子复位或干脆不发送数据。
    config.fOutxCtsFlow = FALSE;
    config.fOutxDsrFlow = FALSE;
    config.fDsrSensitivity = FALSE;
    config.fDtrControl = DTR_CONTROL_ENABLE;
    config.fRtsControl = RTS_CONTROL_ENABLE;
    config.fOutX = FALSE;
    config.fInX = FALSE;
    config.fAbortOnError = FALSE;

    if (SetCommState(handle, &config) == FALSE) {
        last_error_ = "无法设置串口参数：" + describe_error(GetLastError());
        CloseHandle(handle);
        return false;
    }

    // 非阻塞读：ReadFile 立刻返回当前可用数据，不等待。
    // 这样 poll() 可以在主循环里被高频调用而不卡住界面。
    COMMTIMEOUTS timeouts{};
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = 0;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 100;
    if (SetCommTimeouts(handle, &timeouts) == FALSE) {
        last_error_ = "无法设置串口超时：" + describe_error(GetLastError());
        CloseHandle(handle);
        return false;
    }

    SetupComm(handle, 4096, 4096);
    PurgeComm(handle, PURGE_RXCLEAR | PURGE_TXCLEAR);

    impl_->handle = handle;
    port_ = port;
    baud_rate_ = baud_rate;
    return true;
}

void SerialPort::close() {
    if (impl_ && impl_->handle != INVALID_HANDLE_VALUE) {
        CloseHandle(impl_->handle);
        impl_->handle = INVALID_HANDLE_VALUE;
    }
    port_.clear();
    baud_rate_ = 0;
}

bool SerialPort::is_open() const {
    return impl_ && impl_->handle != INVALID_HANDLE_VALUE;
}

std::size_t SerialPort::write(const std::string& data) {
    if (!is_open() || data.empty()) {
        return 0;
    }
    DWORD written = 0;
    if (WriteFile(impl_->handle, data.data(), static_cast<DWORD>(data.size()), &written,
                  nullptr) == FALSE) {
        last_error_ = "写入失败：" + describe_error(GetLastError());
        return 0;
    }
    return written;
}

std::string SerialPort::read() {
    if (!is_open()) {
        return {};
    }

    std::string out;
    std::array<char, 1024> buffer{};
    while (true) {
        DWORD available = 0;
        COMSTAT status{};
        if (ClearCommError(impl_->handle, nullptr, &status) == FALSE) {
            last_error_ = "读取串口状态失败：" + describe_error(GetLastError());
            break;
        }
        if (status.cbInQue == 0) {
            break;
        }
        available = std::min<DWORD>(status.cbInQue, static_cast<DWORD>(buffer.size()));

        DWORD got = 0;
        if (ReadFile(impl_->handle, buffer.data(), available, &got, nullptr) == FALSE || got == 0) {
            break;
        }
        out.append(buffer.data(), got);
        if (got < buffer.size()) {
            break;
        }
    }
    return out;
}

std::string SerialPort::last_error() const {
    return last_error_;
}

}  // namespace handwriting::serial

#endif  // _WIN32
