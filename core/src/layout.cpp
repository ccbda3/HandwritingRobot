#include "handwriting/layout.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <stdexcept>

#include "handwriting/glyph.hpp"

namespace handwriting::layout {

namespace {

constexpr double kDefaultGridCharSize = 20.0;

struct Utf8Char {
    std::string bytes;
    std::size_t offset{0};
};

[[nodiscard]] std::vector<Utf8Char> split_utf8_with_offsets(const std::string& text) {
    std::vector<Utf8Char> chars;
    chars.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        std::size_t length = 1;
        if ((lead & 0x80U) == 0) {
            length = 1;
        } else if ((lead & 0xE0U) == 0xC0U) {
            length = 2;
        } else if ((lead & 0xF0U) == 0xE0U) {
            length = 3;
        } else if ((lead & 0xF8U) == 0xF0U) {
            length = 4;
        }
        if (i + length > text.size()) {
            length = text.size() - i;
        }
        chars.push_back({text.substr(i, length), i});
        i += length;
    }
    return chars;
}

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

Page compute_styled(const std::vector<std::size_t>& row_char_counts,
                    const Settings& settings, const StyleProfile& profile) {
    const double base_size = resolve_char_size(settings);
    if (base_size <= 0.0) {
        throw std::invalid_argument("字宽必须为正");
    }
    const int per_line = resolve_chars_per_line(settings);
    const double gap_mean = std::max(0.0, profile.gap_mean_mm);
    const double size_cv = std::max(0.0, profile.size_cv);
    const double gap_cv = std::max(0.0, profile.gap_cv);
    const double drift = std::max(0.0, profile.baseline_drift_mm);
    const double slope = profile.baseline_slope_mm;

    std::mt19937 rng(profile.seed);
    std::normal_distribution<double> normal(0.0, 1.0);

    Page page;
    page.chars_per_line = per_line;

    if (settings.mode == Mode::Notebook) {
        const double pitch = notebook_pitch(settings);
        const double cell_width = settings.page_width_mm / per_line;
        page.width_mm = settings.page_width_mm;
        page.height_mm = settings.page_height_mm;
        page.rule_lines_mm.reserve(static_cast<std::size_t>(settings.line_count));
        for (int i = 0; i < settings.line_count; ++i) {
            page.rule_lines_mm.push_back((i + 1) * pitch);
        }

        int line = std::max(0, settings.first_line - 1);
        for (const std::size_t row_count : row_char_counts) {
            int column = 0;
            for (std::size_t i = 0; i < row_count; ++i) {
                if (column >= per_line) {
                    ++line;
                    column = 0;
                }
                const double size =
                    base_size * std::clamp(1.0 + size_cv * normal(rng), 0.85, 1.12);
                const double offset = std::clamp(
                    slope * column + drift * normal(rng), -pitch * 0.2, pitch * 0.2);
                const double baseline = (line + 1) * pitch;
                const double x = column * cell_width + (cell_width - size) / 2.0;
                const double y = baseline - glyph::kBaselineRatio * size + offset;
                page.cells.push_back(
                    Cell{x, y, size, y + glyph::kBaselineRatio * size, line, column});
                ++column;
            }
            ++line;
        }
        return page;
    }

    int line = 0;
    double max_right = 0.0;
    double max_bottom = 0.0;
    std::size_t total = 0;
    for (const std::size_t row_count : row_char_counts) {
        total += row_count;
        int column = 0;
        double cursor_x = settings.margin_x_mm;
        for (std::size_t i = 0; i < row_count; ++i) {
            if (column >= per_line) {
                ++line;
                column = 0;
                cursor_x = settings.margin_x_mm;
            }
            const double size =
                base_size * std::clamp(1.0 + size_cv * normal(rng), 0.85, 1.12);
            const double gap =
                std::max(0.0, gap_mean + gap_cv * gap_mean * normal(rng));
            const double offset = std::clamp(
                slope * column + drift * normal(rng), -base_size * 0.2,
                base_size * 0.2);
            const double y = settings.margin_y_mm +
                             line * (base_size + settings.line_gap_mm) + offset;
            page.cells.push_back(
                Cell{cursor_x, y, size, y + glyph::kBaselineRatio * size, line, column});
            max_right = std::max(max_right, cursor_x + size);
            max_bottom = std::max(max_bottom, y + size);
            cursor_x += size + gap;
            ++column;
        }
        ++line;
    }

    const int used_lines = total == 0 ? 0 : std::max(1, line);
    const double nominal_width =
        settings.margin_x_mm * 2.0 + per_line * base_size +
        std::max(0, per_line - 1) * gap_mean;
    const double nominal_height =
        settings.margin_y_mm * 2.0 + used_lines * base_size +
        std::max(0, used_lines - 1) * settings.line_gap_mm;
    page.width_mm = std::max(nominal_width, max_right + settings.margin_x_mm);
    page.height_mm = std::max(nominal_height, max_bottom + settings.margin_y_mm);
    return page;
}

std::vector<PageText> paginate(const std::string& text, const Settings& settings) {
    const int per_line = resolve_chars_per_line(settings);
    int lines_per_page = 1;
    if (settings.mode == Mode::Notebook) {
        lines_per_page =
            std::max(1, settings.line_count - std::max(0, settings.first_line - 1));
    } else {
        const double char_size = resolve_char_size(settings);
        const double usable = settings.page_height_mm - 2.0 * settings.margin_y_mm;
        lines_per_page = std::max(
            1, static_cast<int>(std::floor(
                   usable / std::max(1e-9, char_size + settings.line_gap_mm))));
    }

    const auto chars = split_utf8_with_offsets(text);
    std::vector<PageText> pages;
    PageText page;
    int page_lines = 0;
    bool page_started = false;

    auto finish_page = [&] {
        if (page_started) {
            pages.push_back(std::move(page));
            page = PageText{};
            page_lines = 0;
            page_started = false;
        }
    };

    std::vector<Utf8Char> line;
    std::size_t line_start = 0;

    auto flush_line = [&] {
        const std::size_t per = static_cast<std::size_t>(per_line);
        const int chunks =
            std::max(1, static_cast<int>((line.size() + per - 1) / per));
        for (int c = 0; c < chunks; ++c) {
            const std::size_t begin = static_cast<std::size_t>(c) * per;
            const std::size_t end = std::min(line.size(), begin + per);
            if (page_lines >= lines_per_page) {
                finish_page();
            }
            if (page_lines > 0) {
                page.text += '\n';
            }
            if (!page_started) {
                page.start_offset = line.empty() ? line_start : line[begin].offset;
                page_started = true;
            }
            for (std::size_t i = begin; i < end; ++i) {
                page.text += line[i].bytes;
                page.char_offsets.push_back(line[i].offset);
            }
            ++page_lines;
        }
        line.clear();
    };

    for (const auto& ch : chars) {
        if (ch.bytes == "\n") {
            flush_line();
            line_start = ch.offset + 1;
        } else {
            if (line.empty()) {
                line_start = ch.offset;
            }
            line.push_back(ch);
        }
    }

    const bool ends_with_newline = !text.empty() && text.back() == '\n';
    if (!line.empty() || !ends_with_newline || pages.empty()) {
        flush_line();
    }
    if (page_started) {
        pages.push_back(std::move(page));
    }
    if (pages.empty()) {
        pages.push_back(PageText{});
    }
    return pages;
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
