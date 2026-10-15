// 字形骨架加载 —— C++ 核心库
//
// 与 Python 侧 src/handwriting/glyph.py 行为等价，但有一处刻意不同：
// **C++ 侧不联网**，只从本地缓存目录读取。下载由 Python 侧的
// `handwriting preload` 子命令负责。
//
// 这样核心库不必引入 HTTP 客户端依赖（TLS 库、证书、代理……），
// Qt 应用也不会在运行时因为网络问题卡住。发布版用户拿到的是
// 预置好的字形缓存，完全离线可用。
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "handwriting/json.hpp"
#include "handwriting/trajectory.hpp"

namespace handwriting::glyph {

// Make Me a Hanzi 的 em 框：1024×1024，原点在基线，y 轴向上。
// 标定依据见 docs/glyph-pipeline.md。
inline constexpr double kEmMinX = 0.0;
inline constexpr double kEmMinY = -124.0;
inline constexpr double kEmMaxX = 1024.0;
inline constexpr double kEmMaxY = 900.0;
inline constexpr double kEmSize = kEmMaxX - kEmMinX;

// 基线在归一化坐标中的位置（0.87890625）。
// 字压在横线上时用它从基线反推字格顶部：
//     top_y = baseline_y - kBaselineRatio * char_size
inline constexpr double kBaselineRatio = (kEmMaxY - 0.0) / kEmSize;

inline constexpr double kDefaultResampleStep = 0.01;
inline constexpr int kDefaultSamplesPerSegment = 12;

class GlyphNotFound : public std::runtime_error {
public:
    explicit GlyphNotFound(const std::string& message) : std::runtime_error(message) {}
};

struct Glyph {
    std::string character;
    // 笔画中心线，单位为 Make Me a Hanzi 的 em 单位。外层顺序即书写顺序。
    std::vector<std::vector<std::pair<double, double>>> medians;

    [[nodiscard]] std::size_t stroke_count() const noexcept { return medians.size(); }
    [[nodiscard]] std::size_t point_count() const noexcept;
};

// 从 hanzi-writer 格式的 JSON 构造 Glyph
[[nodiscard]] Glyph parse_glyph(const std::string& character, const json::Value& payload);

// 本地字形缓存目录。文件名用码位（U+6C38.json），
// 避免 Windows 上非 ASCII 文件名带来的编码问题。
class GlyphCache {
public:
    explicit GlyphCache(std::filesystem::path directory);

    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }
    [[nodiscard]] std::filesystem::path path_for(const std::string& character) const;
    [[nodiscard]] bool contains(const std::string& character) const;
    [[nodiscard]] Glyph load(const std::string& character) const;

private:
    std::filesystem::path directory_;
};

// 转换到归一化坐标（0~1，y 向下）并平滑重采样。
// smooth 为 false 时只做坐标转换，用于对比样条平滑的效果。
[[nodiscard]] StrokeSet to_stroke_set(const Glyph& glyph,
                                      double step = kDefaultResampleStep,
                                      int samples_per_segment = kDefaultSamplesPerSegment,
                                      bool smooth = true);

}  // namespace handwriting::glyph
