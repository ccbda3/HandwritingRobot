#include "handwriting/layout.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "handwriting/glyph.hpp"

namespace handwriting::layout {

namespace {

constexpr double kDefaultGridCharSize = 20.0;

[[nodiscard]] double notebook_pitch(const Settings& settings) {
    if (settings.line_count < 1) {
        throw std::invalid_argument("line_count 必须 ≥ 1");
    }
    return settings.page_height_mm / static_cast<double>(settings.line_count);
}

}  // namespace

double resolve_char_size(const Settings& settings) {
    if (settings.char_size_mm > 0.0) {
        return settings.char_size_mm;
    }
    if (settings.mode == Mode::Notebook) {
        // 默认字宽 = 行距：字刚好占满一行，符合作业本上写汉字的观感
        return notebook_pitch(settings);
    }
    return kDefaultGridCharSize;
}

int resolve_chars_per_line(const Settings& settings) {
    if (settings.chars_per_line > 0) {
        return settings.chars_per_line;
    }
    if (settings.mode == Mode::Grid) {
        return std::max(1, settings.grid_chars_per_line);
    }
    const double size = resolve_char_size(settings);
    if (size <= 0.0) {
        return 1;
    }
    return std::max(1, static_cast<int>(std::floor(settings.page_width_mm / size)));
}

Page compute(const std::vector<std::size_t>& row_char_counts, const Settings& settings) {
    const double char_size = resolve_char_size(settings);
    const int per_line = resolve_chars_per_line(settings);
    if (char_size <= 0.0) {
        throw std::invalid_argument("字宽必须为正");
    }

    Page page;
    page.chars_per_line = per_line;

    if (settings.mode == Mode::Notebook) {
        const double pitch = notebook_pitch(settings);
        const double cell_width = settings.page_width_mm / static_cast<double>(per_line);
        page.width_mm = settings.page_width_mm;
        page.height_mm = settings.page_height_mm;

        // 横线：第 n 条线在 n × 行距处（n 从 1 起），共 line_count 条
        page.rule_lines_mm.reserve(static_cast<std::size_t>(settings.line_count));
        for (int i = 1; i <= settings.line_count; ++i) {
            page.rule_lines_mm.push_back(static_cast<double>(i) * pitch);
        }

        int line = std::max(0, settings.first_line - 1);
        for (const std::size_t row_count : row_char_counts) {
            int column = 0;
            for (std::size_t i = 0; i < row_count; ++i) {
                if (column >= per_line) {
                    ++line;
                    column = 0;
                }
                Cell cell;
                cell.size_mm = char_size;
                cell.line = line;
                cell.column = column;
                cell.baseline_mm = static_cast<double>(line + 1) * pitch;
                cell.y_mm = cell.baseline_mm - glyph::kBaselineRatio * char_size;
                cell.x_mm = static_cast<double>(column) * cell_width +
                            (cell_width - char_size) / 2.0;
                page.cells.push_back(cell);
                ++column;
            }
            ++line;  // 显式换行后必定另起一行
        }
        return page;
    }

    // Grid 模式
    const int grid_rows = [&row_char_counts]() {
        std::size_t total = 0;
        for (const std::size_t count : row_char_counts) {
            total += count;
        }
        return total;
    }();

    page.width_mm = settings.margin_x_mm * 2.0 + static_cast<double>(per_line) * char_size +
                    static_cast<double>(std::max(0, per_line - 1)) * settings.char_gap_mm;

    int line = 0;
    for (const std::size_t row_count : row_char_counts) {
        int column = 0;
        for (std::size_t i = 0; i < row_count; ++i) {
            if (column >= per_line) {
                ++line;
                column = 0;
            }
            Cell cell;
            cell.size_mm = char_size;
            cell.line = line;
            cell.column = column;
            cell.x_mm = settings.margin_x_mm +
                        static_cast<double>(column) * (char_size + settings.char_gap_mm);
            cell.y_mm = settings.margin_y_mm +
                        static_cast<double>(line) * (char_size + settings.line_gap_mm);
            cell.baseline_mm = cell.y_mm + glyph::kBaselineRatio * char_size;
            page.cells.push_back(cell);
            ++column;
        }
        ++line;
    }

    const int used_lines = (grid_rows == 0) ? 0 : (line > 0 ? line : 1);
    page.height_mm = settings.margin_y_mm * 2.0 + static_cast<double>(used_lines) * char_size +
                     static_cast<double>(std::max(0, used_lines - 1)) * settings.line_gap_mm;
    return page;
}

Page compute(std::size_t char_count, const Settings& settings) {
    const int per_line = resolve_chars_per_line(settings);
    std::vector<std::size_t> rows;
    std::size_t remaining = char_count;
    while (remaining > 0) {
        const std::size_t take = std::min<std::size_t>(remaining,
                                                       static_cast<std::size_t>(per_line));
        rows.push_back(take);
        remaining -= take;
    }
    if (rows.empty()) {
        rows.push_back(0);
    }
    return compute(rows, settings);
}

}  // namespace handwriting::layout
