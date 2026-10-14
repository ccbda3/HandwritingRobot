// 性能基准：量化「计算」与「串口传输」各自占多少时间。
//
// 目的不是刷分，而是回答一个工程问题：值不值得把某段逻辑从 Python 移植到 C++。
// 判定标准是「移植能省下的时间占端到端总耗时的比例」，而不是单看某段代码快了多少倍。
//
// 与 benchmarks/python/bench_pipeline.py 使用完全相同的合成负载，两者可直接对比。

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "handwriting/gcode.hpp"
#include "handwriting/trajectory.hpp"

using handwriting::GcodeBuilder;
using handwriting::Point;
using handwriting::PlotSettings;
using handwriting::Stroke;
using handwriting::StrokeSet;

namespace {

constexpr int kCharCount = 100;        // 一页 100 字
constexpr int kStrokesPerChar = 8;     // 每字 8 笔
constexpr int kPointsPerStroke = 60;   // 每笔 60 个采样点
constexpr double kGrblLinesPerSecond = 380.0;  // GRBL @ 115200 baud 实测吞吐
constexpr unsigned kSeed = 20261014u;

// 用随机游走而不是独立随机点：真实笔画的相邻采样点是连续的，
// 独立随机点会让路径长度虚高一个数量级，从而把「机床运行时间」算错。
std::vector<std::vector<StrokeSet>> make_page() {
    std::mt19937 rng(kSeed);
    std::uniform_real_distribution<double> start_dist(0.05, 0.95);
    std::uniform_real_distribution<double> step_dist(-0.010, 0.010);
    std::uniform_real_distribution<double> pressure_dist(0.0, 1.0);

    std::vector<StrokeSet> glyphs;
    glyphs.reserve(kCharCount);
    for (int g = 0; g < kCharCount; ++g) {
        std::vector<Stroke> strokes;
        strokes.reserve(kStrokesPerChar);
        for (int s = 0; s < kStrokesPerChar; ++s) {
            double x = start_dist(rng);
            double y = start_dist(rng);
            std::vector<Point> points;
            points.reserve(kPointsPerStroke);
            for (int p = 0; p < kPointsPerStroke; ++p) {
                points.push_back(Point{x, y, pressure_dist(rng), p * 8});
                x = std::clamp(x + step_dist(rng), 0.05, 0.95);
                y = std::clamp(y + step_dist(rng), 0.05, 0.95);
            }
            strokes.emplace_back(std::move(points));
        }
        glyphs.emplace_back(std::move(strokes), "永");
    }

    std::vector<std::vector<StrokeSet>> rows;
    rows.reserve(kCharCount);
    for (int i = 0; i < kCharCount; ++i) {
        rows.push_back({glyphs[static_cast<std::size_t>(i)]});
    }
    return rows;
}

template <typename Fn>
double time_ms(Fn&& fn) {
    const auto start = std::chrono::steady_clock::now();
    fn();
    const auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(end - start).count();
}

}  // namespace

int main() {
    const auto rows = make_page();

    std::size_t total_points = 0;
    for (const auto& row : rows) {
        for (const auto& glyph : row) {
            total_points += glyph.point_count();
        }
    }

    // 1) 归一化：等比缩放到单位方框
    double norm_ms = 0.0;
    std::size_t norm_sink = 0;
    norm_ms = time_ms([&] {
        for (const auto& row : rows) {
            for (const auto& glyph : row) {
                norm_sink += glyph.fit_to_box(1.0).point_count();
            }
        }
    });

    // 2) G 代码生成
    std::string program;
    const double gcode_ms = time_ms([&] { program = handwriting::strokes_to_gcode(rows); });

    GcodeBuilder probe;
    for (const auto& row : rows) {
        for (const auto& glyph : row) {
            for (const auto& stroke : glyph.strokes()) {
                probe.plot_stroke(stroke, 10.0, 10.0, 20.0);
            }
        }
    }
    probe.finish();
    const auto& stats = probe.stats();

    const double serial_seconds = static_cast<double>(probe.lines().size()) / kGrblLinesPerSecond;

    std::printf("\n=== HandwritingRobot C++ 基准 ===\n");
    std::printf("负载           : %d 字 x %d 笔 x %d 点 = %zu 点\n", kCharCount, kStrokesPerChar,
                kPointsPerStroke, total_points);
    std::printf("归一化耗时     : %8.2f ms  (%.1f M点/秒)\n", norm_ms,
                static_cast<double>(total_points) / norm_ms / 1000.0);
    std::printf("G 代码生成耗时 : %8.2f ms  (%zu 行, %.1f M行/秒)\n", gcode_ms, probe.lines().size(),
                static_cast<double>(probe.lines().size()) / gcode_ms / 1000.0);
    std::printf("计算合计       : %8.2f ms\n", norm_ms + gcode_ms);
    std::printf("-------------------------------------------\n");
    std::printf("绘制路径长度   : %8.1f mm\n", stats.draw_length_mm);
    std::printf("空行程长度     : %8.1f mm\n", stats.travel_length_mm);
    std::printf("机床预计运行   : %8.1f s  (仅运动时间)\n", stats.estimated_seconds);
    std::printf("串口传输下限   : %8.1f s  (%zu 行 @ %.0f 行/秒)\n", serial_seconds,
                probe.lines().size(), kGrblLinesPerSecond);
    std::printf("-------------------------------------------\n");
    std::printf("计算占端到端   : %8.3f %%\n",
                100.0 * (norm_ms + gcode_ms) / 1000.0 / (serial_seconds + stats.estimated_seconds));
    std::printf("=> 瓶颈在串口与机械运动，不在计算语言\n\n");
    (void)norm_sink;
    return 0;
}
