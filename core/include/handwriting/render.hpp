// 文本渲染：字形骨架 → 可直接送进 G 代码生成器的轨迹
//
// 与 Python 侧 src/handwriting/render.py 行为等价。
//
// 一处刻意的不一致：抖动（jitter）在两侧使用不同的随机数算法，
// 因此**同一组参数下抖动结果不同**。抖动本身是装饰性的，
// 且 Python 与 C++ 的标准库随机数实现无法逐位对齐，强行对齐没有意义。
// 除抖动外，其余所有几何结果两侧应当完全一致。
#pragma once

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "handwriting/glyph.hpp"
#include "handwriting/trajectory.hpp"

namespace handwriting::render {

struct Settings {
    // 字形倾斜，**右倾为正**。楷书通常 0~5°，行书可达 10° 以上。
    double slant_deg{0.0};

    // 手写抖动强度（高斯标准差，归一化单位）。建议 ≤ 0.005，0 为纯骨架。
    // 注意：抖动**不等于风格**，真正的风格必须由风格模型学习。
    double jitter{0.0};
    std::uint32_t jitter_seed{20261015};

    double resample_step{0.01};
    int samples_per_segment{12};
    bool smooth{true};

    // 提按幅度：笔画两端相对中段重多少。0 为等压。
    double pressure_base{0.45};
    double pressure_amplitude{0.30};

    // 短于该长度的笔画按「点」处理，用单峰压力曲线
    double dot_length_threshold{0.12};

    // 合成时序用的书写速度（归一化单位 / 秒）
    double writing_speed{2.5};
};

struct Result {
    // 每个元素是一行，行内每个元素是一个字。文本里的换行符产生行边界。
    std::vector<std::vector<StrokeSet>> rows;
    std::vector<std::string> missing;
    std::vector<std::string> failures;  // 与 missing 一一对应的原因

    // 与 rows 展平后一一对应：每个成功渲染的字在原文中的 UTF-8 字节偏移。
    // 供 UI 做「点字格改原文」的精确定位（缺字/空白会跳过，不能按序号硬算）。
    std::vector<std::size_t> source_offsets;

    [[nodiscard]] std::size_t char_count() const noexcept;
    [[nodiscard]] bool empty() const noexcept { return char_count() == 0; }
};

// 绕字格中心错切。绕中心而不是绕原点：绕原点会把字形整体推出字格，
// 而且上半与下半的偏移方向相反、幅度还不对称。
[[nodiscard]] StrokeSet apply_slant(const StrokeSet& input, double slant_deg);

[[nodiscard]] StrokeSet apply_jitter(const StrokeSet& input, double amplitude,
                                     std::mt19937& rng);

[[nodiscard]] StrokeSet apply_pressure(const StrokeSet& input, const Settings& settings);

[[nodiscard]] StrokeSet apply_timing(const StrokeSet& input, const Settings& settings);

// 完整渲染一个字。处理顺序有讲究：抖动在倾斜之后（抖动不应被倾斜拉偏），
// 压感与时序在最后（与几何无关，且时序依赖最终几何长度）。
[[nodiscard]] StrokeSet render_glyph(const glyph::Glyph& source, const Settings& settings,
                                     std::mt19937& rng);

// 渲染整段文本。
//
// - 换行符 '\n' 强制断行
// - 空白字符跳过
// - chars_per_line 为 0 时不自动折行
// - 字形缺失的字符被跳过并记录，不影响其余字符
[[nodiscard]] Result render_text(const std::string& text, const glyph::GlyphCache& cache,
                                 const Settings& settings, int chars_per_line);

}  // namespace handwriting::render
