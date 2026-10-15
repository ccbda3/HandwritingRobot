// UTF-8 处理。
//
// 汉字在 UTF-8 里占 3 字节，因此「第 i 个字符」不能等同于「第 i 个字节」。
// 用字节下标切分汉字会把字切成乱码，这类 bug 在中文文本上必现，
// 所以单独成模块，所有涉及字符边界的地方都走这里。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace handwriting::utf8 {

// 解码一个码位。返回 {码位, 该字符占用的字节数}。
// 遇到非法字节序列时返回 {0xFFFD, 1}，即 U+FFFD 替换字符。
struct Decoded {
    std::uint32_t codepoint{0};
    std::size_t length{1};
};

[[nodiscard]] Decoded decode(std::string_view text, std::size_t offset) noexcept;

// 把文本切分成字符（每个元素是一个完整的 UTF-8 字符）
[[nodiscard]] std::vector<std::string> split(std::string_view text);

// 字符个数（不是字节数）
[[nodiscard]] std::size_t count(std::string_view text) noexcept;

// 把码位格式化成 "U+6C38" 形式，用于字形缓存文件名
[[nodiscard]] std::string codepoint_tag(std::uint32_t codepoint);

// 取字符串首字符的码位；空串返回 0
[[nodiscard]] std::uint32_t first_codepoint(std::string_view text) noexcept;

// 判断是否为空白字符（ASCII 空白，以及全角空格 U+3000）
[[nodiscard]] bool is_space(std::uint32_t codepoint) noexcept;

}  // namespace handwriting::utf8
