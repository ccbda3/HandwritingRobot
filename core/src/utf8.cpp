#include "handwriting/utf8.hpp"

#include <cstdio>

namespace handwriting::utf8 {

namespace {
constexpr std::uint32_t kReplacement = 0xFFFD;
}

Decoded decode(std::string_view text, std::size_t offset) noexcept {
    if (offset >= text.size()) {
        return {kReplacement, 1};
    }

    const auto byte = [&text](std::size_t index) {
        return static_cast<std::uint8_t>(text[index]);
    };

    const std::uint8_t first = byte(offset);

    if (first < 0x80) {
        return {first, 1};
    }

    // 2 字节：110xxxxx 10xxxxxx
    if ((first & 0xE0) == 0xC0) {
        if (offset + 1 >= text.size() || (byte(offset + 1) & 0xC0) != 0x80) {
            return {kReplacement, 1};
        }
        const std::uint32_t code =
            (static_cast<std::uint32_t>(first & 0x1F) << 6) | (byte(offset + 1) & 0x3F);
        return {code, 2};
    }

    // 3 字节：1110xxxx 10xxxxxx 10xxxxxx（汉字走这里）
    if ((first & 0xF0) == 0xE0) {
        if (offset + 2 >= text.size() || (byte(offset + 1) & 0xC0) != 0x80 ||
            (byte(offset + 2) & 0xC0) != 0x80) {
            return {kReplacement, 1};
        }
        const std::uint32_t code = (static_cast<std::uint32_t>(first & 0x0F) << 12) |
                                   ((byte(offset + 1) & 0x3F) << 6) | (byte(offset + 2) & 0x3F);
        return {code, 3};
    }

    // 4 字节：11110xxx 10xxxxxx 10xxxxxx 10xxxxxx（增补平面，如 emoji）
    if ((first & 0xF8) == 0xF0) {
        if (offset + 3 >= text.size() || (byte(offset + 1) & 0xC0) != 0x80 ||
            (byte(offset + 2) & 0xC0) != 0x80 || (byte(offset + 3) & 0xC0) != 0x80) {
            return {kReplacement, 1};
        }
        const std::uint32_t code = (static_cast<std::uint32_t>(first & 0x07) << 18) |
                                   ((byte(offset + 1) & 0x3F) << 12) |
                                   ((byte(offset + 2) & 0x3F) << 6) | (byte(offset + 3) & 0x3F);
        return {code, 4};
    }

    return {kReplacement, 1};
}

std::vector<std::string> split(std::string_view text) {
    std::vector<std::string> out;
    std::size_t offset = 0;
    while (offset < text.size()) {
        const Decoded decoded = decode(text, offset);
        out.emplace_back(text.substr(offset, decoded.length));
        offset += decoded.length;
    }
    return out;
}

std::size_t count(std::string_view text) noexcept {
    std::size_t total = 0;
    std::size_t offset = 0;
    while (offset < text.size()) {
        offset += decode(text, offset).length;
        ++total;
    }
    return total;
}

std::string codepoint_tag(std::uint32_t codepoint) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "U+%04X", codepoint);
    return buffer;
}

std::uint32_t first_codepoint(std::string_view text) noexcept {
    if (text.empty()) {
        return 0;
    }
    return decode(text, 0).codepoint;
}

bool is_space(std::uint32_t codepoint) noexcept {
    if (codepoint == ' ' || codepoint == '\t' || codepoint == '\r' || codepoint == '\n' ||
        codepoint == '\v' || codepoint == '\f') {
        return true;
    }
    return codepoint == 0x3000;  // 全角空格
}

}  // namespace handwriting::utf8
