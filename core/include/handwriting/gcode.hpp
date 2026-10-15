// GRBL G 代码生成 —— C++ 核心库
//
// 分段长度与进给速度必须在生成阶段就权衡好，不能交给固件：
//   - GRBL 为梯形速度规划 + junction deviation，look-ahead 仅 16 段，无 S 曲线加加速度限制
//   - 串口 115200 baud 下每秒约可下发 380 行，分段过密会导致规划器饥饿，纸面出现停顿点
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "handwriting/layout.hpp"
#include "handwriting/trajectory.hpp"

namespace handwriting::gcode {

struct PlotSettings {
    // 版面
    double char_size_mm{20.0};
    double char_gap_mm{3.0};
    double line_gap_mm{10.0};
    int chars_per_line{10};
    double margin_x_mm{10.0};
    double margin_y_mm{10.0};

    // 抬落笔
    double pen_up_z{3.0};
    double pen_down_z{0.0};
    bool use_z_axis{true};
    std::string pen_up_cmd{"M5"};
    std::string pen_down_cmd{"M3 S1000"};

    // 运动
    double feed_draw{1200.0};
    double feed_travel{3000.0};

    // 输出
    int decimals{3};
    std::vector<std::string> header{"G21", "G90", "G17"};
    std::vector<std::string> footer{"M5", "G0 X0 Y0"};

    // 单点笔画（点）的驻留时间，秒。不留驻留时间纸面留不下墨点。
    double dot_dwell_s{0.05};
};

struct PlotStats {
    std::size_t stroke_count{0};
    std::size_t point_count{0};
    std::size_t line_count{0};
    double draw_length_mm{0.0};
    double travel_length_mm{0.0};
    double estimated_seconds{0.0};
};

class GcodeBuilder {
public:
    explicit GcodeBuilder(PlotSettings settings = {});

    void plot_stroke(const Stroke& stroke, double origin_x, double origin_y, double size);

    // 抬笔并追加页脚，结束程序。必须在 str() 之前调用一次。
    void finish();

    [[nodiscard]] const PlotStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const std::vector<std::string>& lines() const noexcept { return lines_; }
    [[nodiscard]] std::string str() const;

private:
    void pen_up();
    void pen_down();
    void travel_to(double x, double y);
    void draw_to(double x, double y);
    [[nodiscard]] std::string fmt(double value, int decimals) const;

    PlotSettings settings_;
    std::vector<std::string> lines_;
    PlotStats stats_;
    bool pen_is_down_{false};
};

// 按行分组生成完整 G 代码程序。rows 的每个元素是一行，行内每个元素是一个字。
[[nodiscard]] std::string strokes_to_gcode(const std::vector<std::vector<StrokeSet>>& rows,
                                          const PlotSettings& settings = {});

struct Program {
    std::string text;
    PlotStats stats;
};

// 按版面字格生成 G 代码。
//
// 这是 Qt 前端与命令行共用的正式入口：版面（字格位置、字宽）由
// :mod:`handwriting::layout` 统一计算，本函数只负责把字形摆到字格里
// 并翻译成 G 代码。这样预览画出来的位置与机器实际走的位置必然一致。
//
// glyphs 与 page.cells 按书写顺序一一对应；数量不等时以较少者为准。
[[nodiscard]] Program glyphs_to_gcode(const std::vector<StrokeSet>& glyphs,
                                      const layout::Page& page,
                                      const PlotSettings& settings = {});

}  // namespace handwriting::gcode
