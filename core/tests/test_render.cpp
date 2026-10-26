// 渲染测试：倾斜、抖动、笔压、时序。与 Python 侧 tests/test_render.py 对应。

#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "check.hpp"
#include "handwriting/glyph.hpp"
#include "handwriting/render.hpp"
#include "handwriting/trajectory.hpp"

using handwriting::Point;
using handwriting::Stroke;
using handwriting::StrokeSet;
using handwriting::render::Settings;

namespace {

StrokeSet vertical_stroke() {
    return StrokeSet({Stroke({Point{0.5, 0.0, 0.5, 0}, Point{0.5, 1.0, 0.5, 100}})}, "测");
}

StrokeSet horizontal_stroke(int count = 11) {
    std::vector<Point> points;
    for (int i = 0; i < count; ++i) {
        Point point;
        point.x = static_cast<double>(i) / static_cast<double>(count - 1);
        point.y = 0.5;
        point.pressure = 0.5;
        point.t = i * 10;
        points.push_back(point);
    }
    return StrokeSet({Stroke(std::move(points))}, "测");
}

// 从渲染结果量出倾斜角。与 Python 侧 style._slant_deg 同一套符号约定：
// 右倾为正，只统计偏竖直的笔段，按竖直长度加权。
double measure_slant(const StrokeSet& input) {
    double numerator = 0.0;
    double denominator = 0.0;
    for (const Stroke& stroke : input.strokes()) {
        const std::vector<Point>& points = stroke.points();
        for (std::size_t i = 1; i < points.size(); ++i) {
            const double dx = points[i].x - points[i - 1].x;
            const double dy = points[i].y - points[i - 1].y;
            if (std::fabs(dy) <= std::fabs(dx)) {
                continue;
            }
            const double weight = std::fabs(dy);
            numerator += -std::atan2(dx, dy) * 180.0 / 3.14159265358979323846 * weight;
            denominator += weight;
        }
    }
    return (denominator > 1e-12) ? numerator / denominator : 0.0;
}

}  // namespace

int main() {
    // ---- 倾斜 ----
    {
        const StrokeSet original = vertical_stroke();
        HR_CHECK(handwriting::render::apply_slant(original, 0.0).strokes()[0].points()[0].x ==
                 original.strokes()[0].points()[0].x);
    }
    {
        // 右倾 = 顶偏右、底偏左
        const StrokeSet slanted = handwriting::render::apply_slant(vertical_stroke(), 10.0);
        const std::vector<Point>& points = slanted.strokes()[0].points();
        HR_CHECK(points.front().x > 0.5);
        HR_CHECK(points.back().x < 0.5);
    }
    {
        const StrokeSet slanted = handwriting::render::apply_slant(vertical_stroke(), -10.0);
        const std::vector<Point>& points = slanted.strokes()[0].points();
        HR_CHECK(points.front().x < 0.5);
        HR_CHECK(points.back().x > 0.5);
    }
    {
        // 绕中心错切：中线上的点不移动
        StrokeSet centre({Stroke({Point{0.5, 0.5, 0.5, 0}})});
        HR_CHECK_NEAR(handwriting::render::apply_slant(centre, 15.0).strokes()[0].points()[0].x, 0.5);
    }
    {
        // 符号约定：请求多少度，量出来就该是多少度
        for (const double requested : {-12.0, -5.0, 5.0, 12.0}) {
            const StrokeSet slanted = handwriting::render::apply_slant(vertical_stroke(), requested);
            const double measured = measure_slant(slanted);
            HR_CHECK(std::fabs(measured - requested) < 0.3);
        }
    }

    // ---- 抖动 ----
    {
        std::mt19937 rng(0);
        const StrokeSet original = vertical_stroke();
        HR_CHECK(handwriting::render::apply_jitter(original, 0.0, rng)
                     .strokes()[0]
                     .points()[0]
                     .x == original.strokes()[0].points()[0].x);
    }
    {
        std::mt19937 rng(1);
        const StrokeSet jittered = handwriting::render::apply_jitter(vertical_stroke(), 0.01, rng);
        HR_CHECK(jittered.strokes()[0].points()[0].x != 0.5);
    }
    {
        // 同种子可复现
        std::mt19937 a(42);
        std::mt19937 b(42);
        const StrokeSet first = handwriting::render::apply_jitter(vertical_stroke(), 0.01, a);
        const StrokeSet second = handwriting::render::apply_jitter(vertical_stroke(), 0.01, b);
        HR_CHECK_NEAR(first.strokes()[0].points()[0].x, second.strokes()[0].points()[0].x);
    }
    {
        // 位移的**标准差**应当等于设定的幅度。
        //
        // 不用「每个点的位移都小于 N 倍标准差」这种断言：高斯分布本来就有长尾，
        // 在只有几个采样点的情况下偶发越界是正常的，那样的测试会假失败。
        // 用大量样本统计标准差，既稳健又真正验证了分布的正确性。
        std::mt19937 rng(7);
        const double amplitude = 0.005;
        const int trials = 4000;

        double sum = 0.0;
        double sum_sq = 0.0;
        for (int i = 0; i < trials; ++i) {
            const StrokeSet jittered =
                handwriting::render::apply_jitter(vertical_stroke(), amplitude, rng);
            const double delta = jittered.strokes()[0].points()[0].x - 0.5;
            sum += delta;
            sum_sq += delta * delta;
        }
        const double mean = sum / trials;
        const double variance = sum_sq / trials - mean * mean;
        const double sigma = std::sqrt(variance);

        HR_CHECK(std::fabs(mean) < amplitude * 0.15);          // 均值应接近 0
        HR_CHECK(std::fabs(sigma - amplitude) < amplitude * 0.1);  // 标准差应接近设定值
    }
    {
        // 极端值也应有界：Box-Muller 的 u1 下限是 2^-32，对应约 6.7σ
        std::mt19937 rng(11);
        const double amplitude = 0.005;
        for (int i = 0; i < 500; ++i) {
            const StrokeSet jittered =
                handwriting::render::apply_jitter(vertical_stroke(), amplitude, rng);
            for (const Point& point : jittered.strokes()[0].points()) {
                HR_CHECK(std::fabs(point.x - 0.5) < amplitude * 8.0);
                HR_CHECK(std::fabs(point.y - 0.0) < amplitude * 8.0 + 1.0);
            }
        }
    }

    // ---- 笔压 ----
    {
        // 长笔画：两头重、中段轻
        const StrokeSet result = handwriting::render::apply_pressure(horizontal_stroke(), Settings{});
        const std::vector<Point>& points = result.strokes()[0].points();
        HR_CHECK(points.front().pressure > points[points.size() / 2].pressure);
        HR_CHECK(points.back().pressure > points[points.size() / 2].pressure);
        HR_CHECK_NEAR(points.front().pressure, points.back().pressure);
    }
    {
        // 点：单峰
        std::vector<Point> points;
        for (int i = 0; i < 6; ++i) {
            Point point;
            point.x = 0.5 + i * 0.005;
            point.y = 0.5;
            point.pressure = 0.5;
            point.t = i * 10;
            points.push_back(point);
        }
        StrokeSet dot({Stroke(std::move(points))});
        const StrokeSet result = handwriting::render::apply_pressure(dot, Settings{});
        HR_CHECK(result.strokes()[0].points()[result.strokes()[0].points().size() / 2].pressure >
                 result.strokes()[0].points().front().pressure);
    }
    {
        // 幅度过大时压力被夹在 0~1
        Settings settings;
        settings.pressure_base = 0.9;
        settings.pressure_amplitude = 0.5;
        const StrokeSet result = handwriting::render::apply_pressure(horizontal_stroke(), settings);
        for (const Point& point : result.strokes()[0].points()) {
            HR_CHECK(point.pressure >= 0.0 && point.pressure <= 1.0);
        }
    }
    {
        Settings settings;
        settings.pressure_amplitude = 0.0;
        settings.pressure_base = 0.6;
        const StrokeSet result = handwriting::render::apply_pressure(horizontal_stroke(), settings);
        for (const Point& point : result.strokes()[0].points()) {
            HR_CHECK_NEAR(point.pressure, 0.6);
        }
    }

    // ---- 时序 ----
    {
        // 总长 0.5，速度 2.5 单位/秒 → 200 ms
        StrokeSet stroke({Stroke({Point{0.0, 0.0, 0.5, 0}, Point{0.25, 0.0, 0.5, 0},
                                  Point{0.5, 0.0, 0.5, 0}})});
        Settings settings;
        settings.writing_speed = 2.5;
        const StrokeSet result = handwriting::render::apply_timing(stroke, settings);
        const std::vector<Point>& points = result.strokes()[0].points();
        HR_CHECK(points[0].t == 0);
        HR_CHECK(std::abs(points[1].t - 100) <= 1);
        HR_CHECK(std::abs(points[2].t - 200) <= 1);
    }
    {
        const StrokeSet result = handwriting::render::apply_timing(horizontal_stroke(), Settings{});
        const std::vector<Point>& points = result.strokes()[0].points();
        for (std::size_t i = 1; i < points.size(); ++i) {
            HR_CHECK(points[i].t >= points[i - 1].t);
        }
    }
    {
        Settings slow;
        slow.writing_speed = 1.0;
        Settings fast;
        fast.writing_speed = 4.0;
        StrokeSet stroke({Stroke({Point{0.0, 0.0, 0.5, 0}, Point{1.0, 0.0, 0.5, 0}})});
        HR_CHECK(handwriting::render::apply_timing(stroke, fast).strokes()[0].points().back().t <
                 handwriting::render::apply_timing(stroke, slow).strokes()[0].points().back().t);
    }
    {
        // 每笔内时间从 0 起算，笔间间隔由采集格式的 strokeStartT 表达
        StrokeSet two({Stroke({Point{0.0, 0.0, 0.5, 0}, Point{0.5, 0.0, 0.5, 0}}),
                       Stroke({Point{0.0, 0.5, 0.5, 0}, Point{0.5, 0.5, 0.5, 0}})});
        const StrokeSet result = handwriting::render::apply_timing(two, Settings{});
        for (const Stroke& stroke : result.strokes()) {
            HR_CHECK(stroke.points().front().t == 0);
        }
    }

    {
        // source_offsets：空格跳过，但偏移必须按原文算，UI 改字才能定位
        const auto dir =
            std::filesystem::temp_directory_path() / "hr-render-offsets";
        std::filesystem::create_directories(dir);
        const std::string glyph_json =
            R"({"strokes":["M 0 0 L 10 10 Z"],"medians":[[[428,824],[503,781]]],"radStrokes":[0]})";
        {
            std::ofstream(dir / "U+6625.json") << glyph_json;  // 春
            std::ofstream(dir / "U+7720.json") << glyph_json;  // 眠
        }
        const handwriting::glyph::GlyphCache cache(dir);
        const auto result =
            handwriting::render::render_text("春 眠", cache, Settings{}, 0);
        HR_CHECK(result.rows.size() == 1);
        HR_CHECK(result.source_offsets.size() == 2);
        if (result.source_offsets.size() == 2) {
            HR_CHECK(result.source_offsets[0] == 0);
            HR_CHECK(result.source_offsets[1] == 4);  // 春 3 字节 + 空格 1 字节
        }
        std::error_code error;
        std::filesystem::remove_all(dir, error);
    }

    return hrtest::summary("render");
}
