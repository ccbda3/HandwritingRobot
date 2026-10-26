#include "handwriting/render.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "handwriting/utf8.hpp"

namespace handwriting::render {

namespace {

constexpr double kEps = 1e-9;
constexpr double kPi = 3.14159265358979323846;

[[nodiscard]] double clamp01(double value) noexcept {
    return std::min(1.0, std::max(0.0, value));
}

// StrokeSet 的成员是私有的，只能通过构造函数组装。
// 统一走这个辅助函数，避免每处都重复 move 样板。
[[nodiscard]] StrokeSet make_stroke_set(std::vector<Stroke> strokes, std::string character) {
    return StrokeSet(std::move(strokes), std::move(character));
}

}  // namespace

std::size_t Result::char_count() const noexcept {
    std::size_t total = 0;
    for (const auto& row : rows) {
        total += row.size();
    }
    return total;
}

StrokeSet apply_slant(const StrokeSet& input, double slant_deg) {
    if (std::fabs(slant_deg) < kEps) {
        return input;
    }

    const double factor = std::tan(slant_deg * kPi / 180.0);
    std::vector<Stroke> strokes;
    strokes.reserve(input.stroke_count());

    for (const Stroke& stroke : input.strokes()) {
        std::vector<Point> points = stroke.points();
        for (Point& point : points) {
            point.x = point.x - factor * (point.y - 0.5);
        }
        strokes.emplace_back(std::move(points));
    }
    return make_stroke_set(std::move(strokes), input.character());
}

StrokeSet apply_jitter(const StrokeSet& input, double amplitude, std::mt19937& rng) {
    if (amplitude <= 0.0) {
        return input;
    }

    // 自实现 Box-Muller 而不是 std::normal_distribution：
    // 后者的分布映射由实现定义，换编译器就会得到不同结果。
    // 这里用 mt19937（算法已标准化）配合固定的均匀化与变换，至少做到同平台可复现。
    //
    // 注意 mt19937 输出 32 位，因此除数是 2^32 而不是 2^53。
    // 按 64 位写（右移 11 位）会让均匀数落在 [0, 2^-32]，
    // 抖动幅度缩小到几乎为零，而且完全看不出错。
    constexpr double kScale = 1.0 / 4294967296.0;  // 2^32
    const auto uniform = [&rng]() { return static_cast<double>(rng()) * kScale; };
    const auto normal = [&uniform]() {
        // 1 - u 使 u1 落在 (0, 1]，避免 log(0)
        const double u1 = 1.0 - uniform();
        const double u2 = uniform();
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u2);
    };

    std::vector<Stroke> strokes;
    strokes.reserve(input.stroke_count());

    for (const Stroke& stroke : input.strokes()) {
        std::vector<Point> points = stroke.points();
        for (Point& point : points) {
            point.x += normal() * amplitude;
            point.y += normal() * amplitude;
        }
        strokes.emplace_back(std::move(points));
    }
    return make_stroke_set(std::move(strokes), input.character());
}

StrokeSet apply_pressure(const StrokeSet& input, const Settings& settings) {
    std::vector<Stroke> strokes;
    strokes.reserve(input.stroke_count());

    for (const Stroke& stroke : input.strokes()) {
        std::vector<Point> points = stroke.points();
        const std::size_t count = points.size();
        const bool is_dot = stroke.length() < settings.dot_length_threshold;

        for (std::size_t i = 0; i < count; ++i) {
            const double t =
                (count > 1) ? static_cast<double>(i) / static_cast<double>(count - 1) : 0.5;
            // 长笔画（横、竖）：两头重、中段轻，即起笔顿、行笔提、收笔顿
            // 短笔画（点）：单峰，中段最重
            const double shape = is_dot ? std::sin(kPi * t) : 1.0 - std::sin(kPi * t);
            points[i].pressure =
                clamp01(settings.pressure_base + settings.pressure_amplitude * shape);
        }
        strokes.emplace_back(std::move(points));
    }
    return make_stroke_set(std::move(strokes), input.character());
}

StrokeSet apply_timing(const StrokeSet& input, const Settings& settings) {
    if (settings.writing_speed <= 0.0) {
        throw std::invalid_argument("writing_speed 必须为正");
    }

    std::vector<Stroke> strokes;
    strokes.reserve(input.stroke_count());

    for (const Stroke& stroke : input.strokes()) {
        std::vector<Point> points = stroke.points();
        double elapsed_ms = 0.0;

        for (std::size_t i = 0; i < points.size(); ++i) {
            if (i > 0) {
                const double dx = points[i].x - points[i - 1].x;
                const double dy = points[i].y - points[i - 1].y;
                const double step = std::sqrt(dx * dx + dy * dy);
                elapsed_ms += step / settings.writing_speed * 1000.0;
            }
            points[i].t = static_cast<std::int32_t>(std::lround(elapsed_ms));
        }
        strokes.emplace_back(std::move(points));
    }
    return make_stroke_set(std::move(strokes), input.character());
}

StrokeSet render_glyph(const glyph::Glyph& source, const Settings& settings, std::mt19937& rng) {
    StrokeSet result = glyph::to_stroke_set(source, settings.resample_step,
                                            settings.samples_per_segment, settings.smooth);
    // 顺序有讲究：抖动在倾斜之后（抖动不应被倾斜拉偏），
    // 压感与时序在最后（与几何无关，且时序依赖最终几何长度）
    result = apply_slant(result, settings.slant_deg);
    result = apply_jitter(result, settings.jitter, rng);
    result = apply_pressure(result, settings);
    return apply_timing(result, settings);
}

Result render_text(const std::string& text, const glyph::GlyphCache& cache,
                   const Settings& settings, int chars_per_line) {
    if (chars_per_line < 0) {
        throw std::invalid_argument("chars_per_line 不能为负");
    }

    Result result;
    std::mt19937 rng(settings.jitter_seed);
    std::vector<StrokeSet> current;

    const auto flush = [&result, &current]() {
        if (!current.empty()) {
            result.rows.push_back(current);
            current.clear();
        }
    };

    std::size_t offset = 0;
    for (const std::string& character : utf8::split(text)) {
        const std::size_t char_offset = offset;
        offset += character.size();

        if (character == "\n") {
            flush();
            continue;
        }
        if (chars_per_line > 0 && static_cast<int>(current.size()) >= chars_per_line) {
            flush();
        }
        if (utf8::is_space(utf8::first_codepoint(character))) {
            // 空白保留为一个空字格：排版会占位，预览/G 代码不画任何笔画。
            current.push_back(StrokeSet({}, " "));
            result.source_offsets.push_back(char_offset);
            continue;
        }

        try {
            current.push_back(render_glyph(cache.load(character), settings, rng));
            result.source_offsets.push_back(char_offset);
        } catch (const std::exception& error) {
            if (std::find(result.missing.begin(), result.missing.end(), character) ==
                result.missing.end()) {
                result.missing.push_back(character);
                result.failures.emplace_back(error.what());
            }
        }
    }
    flush();
    return result;
}

}  // namespace handwriting::render
