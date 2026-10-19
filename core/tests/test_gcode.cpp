// G 代码生成测试。与 Python 侧 tests/test_gcode.py 逐条对应。

#include "check.hpp"
#include "handwriting/gcode.hpp"

#include <cmath>
#include <string>
#include <vector>

using handwriting::gcode::PlotSettings;
using handwriting::Point;
using handwriting::Stroke;
using handwriting::StrokeSet;
using handwriting::gcode::strokes_to_gcode;

namespace {

StrokeSet single_char() {
    return StrokeSet({Stroke({Point{0.1, 0.2}, Point{0.3, 0.4}})});
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::string current;
    for (const char c : text) {
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty()) {
        lines.push_back(current);
    }
    return lines;
}

int count_line(const std::vector<std::string>& lines, const std::string& target) {
    int total = 0;
    for (const auto& line : lines) {
        if (line == target) {
            ++total;
        }
    }
    return total;
}

int index_of(const std::vector<std::string>& lines, const std::string& target) {
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i] == target) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::vector<std::string> travels_of(const std::vector<std::string>& lines) {
    std::vector<std::string> travels;
    for (const auto& line : lines) {
        if (line.rfind("G0 X", 0) == 0 && line != "G0 X0 Y0") {
            travels.push_back(line);
        }
    }
    return travels;
}

}  // namespace

int main() {
    // ---- 程序结构 ----
    const auto lines = split_lines(strokes_to_gcode({{single_char()}}));
    HR_CHECK(lines.size() == 9);
    HR_CHECK(lines[0] == "G21");
    HR_CHECK(lines[1] == "G90");
    HR_CHECK(lines[2] == "G17");
    HR_CHECK(lines[lines.size() - 2] == "M5");
    HR_CHECK(lines[lines.size() - 1] == "G0 X0 Y0");

    HR_CHECK(count_line(lines, "G0 Z0") == 1);
    HR_CHECK(count_line(lines, "G0 Z3") == 1);
    HR_CHECK(index_of(lines, "G0 Z0") < index_of(lines, "G1 X16 Y18 F1200"));
    HR_CHECK(index_of(lines, "G0 Z3") > index_of(lines, "G1 X16 Y18 F1200"));
    HR_CHECK(lines[3] == "G0 X12 Y14");

    // ---- 版面 ----
    // 20mm 字宽 + 3mm 间距 => 第二个字原点 x = 10 + 23 = 33，首点 x = 35
    const auto two_cols = travels_of(split_lines(strokes_to_gcode({{single_char(), single_char()}})));
    HR_CHECK(two_cols.size() == 2);
    HR_CHECK(two_cols[0] == "G0 X12 Y14");
    HR_CHECK(two_cols[1] == "G0 X35 Y14");

    // 换行偏移 = 20mm 字高 + 10mm 行距
    const auto two_rows = travels_of(split_lines(strokes_to_gcode({{single_char()}, {single_char()}})));
    HR_CHECK(two_rows.size() == 2);
    HR_CHECK(two_rows[1] == "G0 X12 Y44");

    // ---- Y 轴翻转（机器 Y+ 朝上时用）----
    handwriting::gcode::PlotSettings flipped;
    flipped.invert_y = true;
    const auto flipped_travels =
        travels_of(split_lines(strokes_to_gcode({{single_char()}}, flipped)));
    HR_CHECK(flipped_travels.size() == 1);
    HR_CHECK(flipped_travels[0] == "G0 X12 Y-14");

    // ---- 统计 ----
    handwriting::gcode::GcodeBuilder builder;
    builder.plot_stroke(single_char().strokes().front(), 10.0, 10.0, 20.0);
    builder.finish();
    HR_CHECK(builder.stats().stroke_count == 1);
    HR_CHECK(builder.stats().point_count == 2);
    // 行数用于估算串口传输时间 —— 那是整条链路的真实瓶颈
    HR_CHECK(builder.stats().line_count == builder.lines().size());
    HR_CHECK_NEAR(builder.stats().draw_length_mm, std::sqrt(32.0));
    HR_CHECK_NEAR(builder.stats().travel_length_mm, std::sqrt(20.0));
    HR_CHECK_NEAR(builder.stats().estimated_seconds,
                  std::sqrt(32.0) / 20.0 + std::sqrt(20.0) / 50.0);

    // ---- 单点笔画：落笔 + 驻留 + 抬笔 ----
    const StrokeSet dotted({Stroke({Point{0.1, 0.1}}), Stroke({Point{0.9, 0.9}})});
    const auto dot_lines = split_lines(strokes_to_gcode({{dotted}}));
    HR_CHECK(count_line(dot_lines, "G0 Z0") == 2);
    HR_CHECK(count_line(dot_lines, "G0 Z3") == 2);
    HR_CHECK(count_line(dot_lines, "G4 P0.05") == 2);

    // ---- 主轴（舵机 / 电磁铁）模式 ----
    PlotSettings spindle;
    spindle.use_z_axis = false;
    const auto spindle_lines = split_lines(strokes_to_gcode({{single_char()}}, spindle));
    HR_CHECK(count_line(spindle_lines, "M3 S1000") == 1);
    HR_CHECK(count_line(spindle_lines, "G0 Z0") == 0);
    // 抬笔本身是 M5，页脚不应再补一个，否则出现连续两个 M5
    for (std::size_t i = 0; i + 1 < spindle_lines.size(); ++i) {
        HR_CHECK(!(spindle_lines[i] == "M5" && spindle_lines[i + 1] == "M5"));
    }

    // ---- 进给速度与小数位 ----
    PlotSettings fast;
    fast.feed_draw = 600.0;
    const std::string fast_text = strokes_to_gcode({{single_char()}}, fast);
    HR_CHECK(fast_text.find("F600") != std::string::npos);
    HR_CHECK(fast_text.find("X12 ") != std::string::npos);  // 12.000 应输出为 12

    // ---- 空输入也要产出合法程序 ----
    const std::vector<std::vector<StrokeSet>> empty;
    const auto empty_lines = split_lines(strokes_to_gcode(empty));
    HR_CHECK(empty_lines.size() == 5);
    HR_CHECK(empty_lines[0] == "G21");
    HR_CHECK(empty_lines[4] == "G0 X0 Y0");

    return hrtest::summary("gcode");
}
