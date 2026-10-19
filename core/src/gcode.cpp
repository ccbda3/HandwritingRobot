#include "handwriting/gcode.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <utility>

namespace handwriting::gcode {

namespace {

double distance(double x1, double y1, double x2, double y2) {
    const double dx = x2 - x1;
    const double dy = y2 - y1;
    return std::sqrt(dx * dx + dy * dy);
}

}  // namespace

GcodeBuilder::GcodeBuilder(PlotSettings settings) : settings_(std::move(settings)) {
    lines_.reserve(1024);
    for (const auto& line : settings_.header) {
        lines_.push_back(line);
    }
}

std::string GcodeBuilder::fmt(double value, int decimals) const {
    // 用 snprintf 而不是 ostringstream：G 代码生成是逐点调用，
    // iostream 的虚拟分发与 locale 处理在这个调用频率下会成为主要开销。
    char buffer[32];
    const int written = std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    if (written <= 0) {
        return "0";
    }

    std::string text(buffer, static_cast<std::size_t>(written));
    if (text.find('.') != std::string::npos) {
        while (text.size() > 1 && text.back() == '0') {
            text.pop_back();
        }
        if (!text.empty() && text.back() == '.') {
            text.pop_back();
        }
    }
    if (text == "-0") {
        text = "0";
    }
    return text;
}

void GcodeBuilder::pen_up() {
    if (!pen_is_down_) {
        return;
    }
    pen_is_down_ = false;
    if (settings_.use_z_axis) {
        lines_.push_back("G0 Z" + fmt(settings_.pen_up_z, settings_.decimals));
    } else {
        lines_.push_back(settings_.pen_up_cmd);
    }
}

void GcodeBuilder::pen_down() {
    if (pen_is_down_) {
        return;
    }
    pen_is_down_ = true;
    if (settings_.use_z_axis) {
        lines_.push_back("G0 Z" + fmt(settings_.pen_down_z, settings_.decimals));
    } else {
        lines_.push_back(settings_.pen_down_cmd);
    }
}

void GcodeBuilder::travel_to(double x, double y) {
    pen_up();
    const double out_y = settings_.invert_y ? -y : y;
    lines_.push_back("G0 X" + fmt(x, settings_.decimals) + " Y" +
                     fmt(out_y, settings_.decimals));
}

void GcodeBuilder::draw_to(double x, double y) {
    pen_down();
    const double out_y = settings_.invert_y ? -y : y;
    lines_.push_back("G1 X" + fmt(x, settings_.decimals) + " Y" +
                     fmt(out_y, settings_.decimals) + " F" + fmt(settings_.feed_draw, 0));
}

void GcodeBuilder::plot_stroke(const Stroke& stroke, double origin_x, double origin_y,
                               double size) {
    if (stroke.empty()) {
        return;
    }
    const auto& points = stroke.points();
    ++stats_.stroke_count;
    stats_.point_count += points.size();

    const double first_x = origin_x + points.front().x * size;
    const double first_y = origin_y + points.front().y * size;
    travel_to(first_x, first_y);
    stats_.travel_length_mm += distance(origin_x, origin_y, first_x, first_y);

    if (points.size() == 1) {
        // 单点笔画：落笔后驻留一小段时间，再抬笔，在纸面留下一个墨点
        pen_down();
        lines_.push_back("G4 P" + fmt(settings_.dot_dwell_s, 2));
        pen_up();
        return;
    }

    double prev_x = first_x;
    double prev_y = first_y;
    for (std::size_t i = 1; i < points.size(); ++i) {
        const double x = origin_x + points[i].x * size;
        const double y = origin_y + points[i].y * size;
        stats_.draw_length_mm += distance(prev_x, prev_y, x, y);
        draw_to(x, y);
        prev_x = x;
        prev_y = y;
    }
    pen_up();
}

void GcodeBuilder::finish() {
    pen_up();
    for (const auto& line : settings_.footer) {
        // 跳过与上一行重复的页脚指令：主轴模式下抬笔本身就是 M5，
        // 页脚再写一次会输出两个连续的 M5（无害但脏）
        if (lines_.empty() || lines_.back() != line) {
            lines_.push_back(line);
        }
    }
    stats_.line_count = lines_.size();
    stats_.estimated_seconds =
        stats_.draw_length_mm / (settings_.feed_draw / 60.0) +
        stats_.travel_length_mm / (settings_.feed_travel / 60.0);
}

std::string GcodeBuilder::str() const {
    std::string out;
    out.reserve(lines_.size() * 24);
    for (const auto& line : lines_) {
        out += line;
        out += '\n';
    }
    return out;
}

std::string strokes_to_gcode(const std::vector<std::vector<StrokeSet>>& rows,
                             const PlotSettings& settings) {
    GcodeBuilder builder(settings);
    for (std::size_t row = 0; row < rows.size(); ++row) {
        for (std::size_t col = 0; col < rows[row].size(); ++col) {
            const double origin_x =
                settings.margin_x_mm + static_cast<double>(col) * (settings.char_size_mm +
                                                                   settings.char_gap_mm);
            const double origin_y =
                settings.margin_y_mm + static_cast<double>(row) * (settings.char_size_mm +
                                                                   settings.line_gap_mm);
            for (const auto& stroke : rows[row][col].strokes()) {
                builder.plot_stroke(stroke, origin_x, origin_y, settings.char_size_mm);
            }
        }
    }
    builder.finish();
    return builder.str();
}

Program glyphs_to_gcode(const std::vector<StrokeSet>& glyphs, const layout::Page& page,
                        const PlotSettings& settings) {
    GcodeBuilder builder(settings);
    const std::size_t count = std::min(glyphs.size(), page.cells.size());

    for (std::size_t i = 0; i < count; ++i) {
        const layout::Cell& cell = page.cells[i];
        for (const Stroke& stroke : glyphs[i].strokes()) {
            builder.plot_stroke(stroke, cell.x_mm, cell.y_mm, cell.size_mm);
        }
    }

    builder.finish();
    return Program{builder.str(), builder.stats()};
}

}  // namespace handwriting::gcode
