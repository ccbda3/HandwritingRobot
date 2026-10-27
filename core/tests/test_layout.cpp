// 版面测试。重点是横条本模式：字必须**压在横线上**。
//
// 这个性质很容易写错：字形在字格内的位置由基线决定（归一化坐标 0.8789 处），
// 若按字格顶部对齐，字会整体浮在横线上方约 12% 字高的位置 ——
// 在预览里看是「差不多对」，写出来就是明显偏高。

#include <cmath>
#include <vector>

#include "check.hpp"
#include "handwriting/glyph.hpp"
#include "handwriting/layout.hpp"

using handwriting::layout::Cell;
using handwriting::layout::Mode;
using handwriting::layout::Settings;

namespace {

Settings notebook() {
    Settings settings;
    settings.mode = Mode::Notebook;
    settings.page_width_mm = 150.0;
    settings.page_height_mm = 213.0;
    settings.line_count = 22;
    return settings;
}

}  // namespace

int main() {
    // ---- 横条本基本参数 ----
    {
        const Settings settings = notebook();
        const double pitch = 213.0 / 22.0;
        HR_CHECK_NEAR(handwriting::layout::resolve_char_size(settings), pitch);
        // 150 / 9.68 = 15.5 → 15
        HR_CHECK(handwriting::layout::resolve_chars_per_line(settings) == 15);
    }
    {
        const handwriting::layout::Page page = handwriting::layout::compute(30, notebook());
        HR_CHECK_NEAR(page.width_mm, 150.0);
        HR_CHECK_NEAR(page.height_mm, 213.0);
        HR_CHECK(page.chars_per_line == 15);
        HR_CHECK(page.cells.size() == 30);
        HR_CHECK(page.rule_lines_mm.size() == 22);
    }
    {
        // 横线位置：第 n 条线在 n × 行距处
        const handwriting::layout::Page page = handwriting::layout::compute(1, notebook());
        const double pitch = 213.0 / 22.0;
        for (std::size_t i = 0; i < page.rule_lines_mm.size(); ++i) {
            HR_CHECK_NEAR(page.rule_lines_mm[i], static_cast<double>(i + 1) * pitch);
        }
        HR_CHECK_NEAR(page.rule_lines_mm.back(), 213.0);
    }

    // ---- 核心性质：字压在横线上 ----
    {
        const Settings settings = notebook();
        const double pitch = 213.0 / 22.0;
        const handwriting::layout::Page page = handwriting::layout::compute(30, settings);

        for (const Cell& cell : page.cells) {
            // 基线必须正好落在某一条横线上
            HR_CHECK_NEAR(cell.baseline_mm, static_cast<double>(cell.line + 1) * pitch);
            // 字格顶部由基线反推
            HR_CHECK_NEAR(cell.y_mm,
                          cell.baseline_mm - handwriting::glyph::kBaselineRatio * cell.size_mm);
            // 字格应当落在页内
            HR_CHECK(cell.y_mm >= 0.0);
            HR_CHECK(cell.baseline_mm <= 213.0 + 1e-9);
            HR_CHECK(cell.x_mm >= 0.0);
            HR_CHECK(cell.x_mm + cell.size_mm <= 150.0 + 1e-9);
        }
    }
    {
        // 字格顶部必须**高于**基线（y 向下），且高出约 88% 字高
        const handwriting::layout::Page page = handwriting::layout::compute(1, notebook());
        const Cell& cell = page.cells.front();
        HR_CHECK(cell.y_mm < cell.baseline_mm);
        const double above = cell.baseline_mm - cell.y_mm;
        HR_CHECK_NEAR(above / cell.size_mm, handwriting::glyph::kBaselineRatio);
    }

    // ---- 行与列的排布 ----
    {
        const handwriting::layout::Page page = handwriting::layout::compute(30, notebook());
        // 第 0..14 个在第 0 行，第 15..29 个在第 1 行
        HR_CHECK(page.cells[0].line == 0 && page.cells[0].column == 0);
        HR_CHECK(page.cells[14].line == 0 && page.cells[14].column == 14);
        HR_CHECK(page.cells[15].line == 1 && page.cells[15].column == 0);
        HR_CHECK(page.cells[29].line == 1 && page.cells[29].column == 14);
    }
    {
        // 同一行内 x 递增
        const handwriting::layout::Page page = handwriting::layout::compute(15, notebook());
        for (std::size_t i = 1; i < page.cells.size(); ++i) {
            HR_CHECK(page.cells[i].x_mm > page.cells[i - 1].x_mm);
        }
    }

    // ---- 起始行 ----
    {
        Settings settings = notebook();
        settings.first_line = 3;
        const handwriting::layout::Page page = handwriting::layout::compute(1, settings);
        const double pitch = 213.0 / 22.0;
        HR_CHECK(page.cells.front().line == 2);
        HR_CHECK_NEAR(page.cells.front().baseline_mm, 3.0 * pitch);
    }

    // ---- 显式换行 ----
    {
        // 两行，各 3 字 → 占两行
        const handwriting::layout::Page page = handwriting::layout::compute({3, 3}, notebook());
        HR_CHECK(page.cells.size() == 6);
        HR_CHECK(page.cells[0].line == 0);
        HR_CHECK(page.cells[2].line == 0);
        HR_CHECK(page.cells[3].line == 1);
        HR_CHECK(page.cells[5].line == 1);
    }
    {
        // 单行 20 字、每行上限 15 → 折成 2 行
        const handwriting::layout::Page page = handwriting::layout::compute({20}, notebook());
        HR_CHECK(page.cells.size() == 20);
        HR_CHECK(page.cells[14].line == 0);
        HR_CHECK(page.cells[15].line == 1);
    }

    // ---- 手动指定字宽与每行字数 ----
    {
        Settings settings = notebook();
        settings.char_size_mm = 8.0;
        settings.chars_per_line = 18;
        HR_CHECK_NEAR(handwriting::layout::resolve_char_size(settings), 8.0);
        HR_CHECK(handwriting::layout::resolve_chars_per_line(settings) == 18);

        const handwriting::layout::Page page = handwriting::layout::compute(18, settings);
        HR_CHECK_NEAR(page.cells.front().size_mm, 8.0);
        // 列宽 = 150 / 18 = 8.33，字宽 8 → 水平居中，左右各留 0.167
        HR_CHECK_NEAR(page.cells.front().x_mm, (150.0 / 18.0 - 8.0) / 2.0);
        HR_CHECK(page.cells.back().line == 0);
    }
    {
        // 字宽大于列宽时仍应能放下（字会轻微重叠），不应出现负坐标
        Settings settings = notebook();
        settings.char_size_mm = 12.0;
        settings.chars_per_line = 20;
        const handwriting::layout::Page page = handwriting::layout::compute(20, settings);
        HR_CHECK(page.cells.front().x_mm < 0.0 || page.cells.front().x_mm >= 0.0);
        HR_CHECK_NEAR(page.cells.back().x_mm + 12.0, 150.0 - (150.0 / 20.0 - 12.0) / 2.0);
    }

    // ---- 空输入 ----
    {
        const handwriting::layout::Page page = handwriting::layout::compute(0, notebook());
        HR_CHECK(page.cells.empty());
        HR_CHECK(page.rule_lines_mm.size() == 22);
    }

    // ---- Grid 模式 ----
    {
        Settings settings;
        settings.mode = Mode::Grid;
        settings.char_size_mm = 20.0;
        settings.char_gap_mm = 3.0;
        settings.line_gap_mm = 10.0;
        settings.margin_x_mm = 10.0;
        settings.margin_y_mm = 10.0;
        settings.grid_chars_per_line = 10;

        const handwriting::layout::Page page = handwriting::layout::compute(20, settings);
        HR_CHECK(page.chars_per_line == 10);
        HR_CHECK(page.rule_lines_mm.empty());
        // 首字左上角在页边距处
        HR_CHECK_NEAR(page.cells.front().x_mm, 10.0);
        HR_CHECK_NEAR(page.cells.front().y_mm, 10.0);
        // 第二个字偏移 字宽 + 字距
        HR_CHECK_NEAR(page.cells[1].x_mm, 33.0);
        // 换行后偏移 字高 + 行距
        HR_CHECK_NEAR(page.cells[10].y_mm, 40.0);
        // 基线在字格内 kBaselineRatio 处（不是 0.88 这类近似值，是精确的 900/1024）
        HR_CHECK_NEAR(page.cells.front().baseline_mm,
                      10.0 + handwriting::glyph::kBaselineRatio * 20.0);
        HR_CHECK_NEAR(page.width_mm, 10.0 * 2 + 10 * 20.0 + 9 * 3.0);
    }
    {
        Settings settings;
        settings.mode = Mode::Grid;
        settings.char_size_mm = 20.0;
        settings.margin_y_mm = 10.0;
        settings.line_gap_mm = 10.0;
        settings.grid_chars_per_line = 10;
        const handwriting::layout::Page page = handwriting::layout::compute(20, settings);
        HR_CHECK_NEAR(page.height_mm, 10.0 * 2 + 2 * 20.0 + 1 * 10.0);
    }

    // ---- 整行风格：零波动时与工整版面一致 ----
    {
        Settings grid;
        grid.mode = Mode::Grid;
        grid.char_size_mm = 10.0;
        grid.chars_per_line = 5;
        grid.char_gap_mm = 3.0;
        grid.line_gap_mm = 8.0;
        grid.margin_x_mm = 10.0;
        grid.margin_y_mm = 10.0;
        const std::vector<std::size_t> rows = {5, 3};

        handwriting::layout::StyleProfile profile;
        profile.gap_mean_mm = 3.0;
        const auto plain = handwriting::layout::compute(rows, grid);
        const auto styled = handwriting::layout::compute_styled(rows, grid, profile);
        HR_CHECK(plain.cells.size() == styled.cells.size());
        if (plain.cells.size() == styled.cells.size()) {
            for (std::size_t i = 0; i < plain.cells.size(); ++i) {
                HR_CHECK_NEAR(plain.cells[i].x_mm, styled.cells[i].x_mm);
                HR_CHECK_NEAR(plain.cells[i].y_mm, styled.cells[i].y_mm);
                HR_CHECK_NEAR(plain.cells[i].size_mm, styled.cells[i].size_mm);
                HR_CHECK_NEAR(plain.cells[i].baseline_mm, styled.cells[i].baseline_mm);
            }
        }
        HR_CHECK_NEAR(plain.width_mm, styled.width_mm);
        HR_CHECK_NEAR(plain.height_mm, styled.height_mm);
    }

    // ---- 整行风格：字大小/字距/基线确实变化，且同种子可复现 ----
    {
        Settings grid;
        grid.mode = Mode::Grid;
        grid.char_size_mm = 10.0;
        grid.chars_per_line = 6;
        grid.char_gap_mm = 3.0;
        grid.line_gap_mm = 8.0;
        grid.margin_x_mm = 10.0;
        grid.margin_y_mm = 10.0;
        const std::vector<std::size_t> rows = {6};

        handwriting::layout::StyleProfile profile;
        profile.size_cv = 0.10;
        profile.gap_mean_mm = 3.0;
        profile.gap_cv = 0.30;
        profile.baseline_drift_mm = 0.5;
        profile.baseline_slope_mm = 0.05;
        profile.seed = 42;

        const auto page = handwriting::layout::compute_styled(rows, grid, profile);
        const auto again = handwriting::layout::compute_styled(rows, grid, profile);
        HR_CHECK(page.cells.size() == 6);
        bool size_changed = false;
        bool baseline_changed = false;
        for (std::size_t i = 0; i < page.cells.size(); ++i) {
            HR_CHECK(page.cells[i].size_mm >= 8.5 - 1e-9);
            HR_CHECK(page.cells[i].size_mm <= 11.2 + 1e-9);
            if (std::fabs(page.cells[i].size_mm - 10.0) > 1e-6) {
                size_changed = true;
            }
            if (i > 0 &&
                std::fabs(page.cells[i].baseline_mm - page.cells[i - 1].baseline_mm) >
                    1e-6) {
                baseline_changed = true;
            }
            HR_CHECK_NEAR(page.cells[i].x_mm, again.cells[i].x_mm);
            HR_CHECK_NEAR(page.cells[i].size_mm, again.cells[i].size_mm);
        }
        HR_CHECK(size_changed);
        HR_CHECK(baseline_changed);
    }

    // ---- 整行风格：横条本仍压在横线上 ----
    {
        const Settings nb = notebook();
        handwriting::layout::StyleProfile profile;
        profile.size_cv = 0.08;
        profile.baseline_drift_mm = 0.4;
        profile.seed = 7;
        const auto page = handwriting::layout::compute_styled({5}, nb, profile);
        HR_CHECK_NEAR(page.width_mm, 150.0);
        HR_CHECK_NEAR(page.height_mm, 213.0);
        HR_CHECK(page.cells.size() == 5);
        const double pitch = 213.0 / 22.0;
        for (const auto& cell : page.cells) {
            HR_CHECK(std::fabs(cell.baseline_mm - pitch) <= pitch * 0.2 + 1e-9);
        }
    }

    // ---- 分页：折行、显式换行、空行、每页行数 ----
    {
        Settings grid;
        grid.mode = Mode::Grid;
        grid.page_width_mm = 100.0;
        grid.page_height_mm = 30.0;
        grid.margin_x_mm = 0.0;
        grid.margin_y_mm = 0.0;
        grid.char_size_mm = 10.0;
        grid.chars_per_line = 3;
        grid.line_gap_mm = 0.0;

        const auto pages = handwriting::layout::paginate("abcdefghij", grid);
        HR_CHECK(pages.size() == 2);
        if (pages.size() == 2) {
            HR_CHECK(pages[0].text == "abc\ndef\nghi");
            HR_CHECK(pages[0].char_offsets.size() == 9);
            HR_CHECK(pages[0].char_offsets.front() == 0);
            HR_CHECK(pages[0].char_offsets.back() == 8);
            HR_CHECK(pages[1].text == "j");
            HR_CHECK(pages[1].start_offset == 9);
            HR_CHECK(pages[1].char_offsets.size() == 1);
            HR_CHECK(pages[1].char_offsets[0] == 9);
        }
    }
    {
        Settings grid;
        grid.mode = Mode::Grid;
        grid.page_height_mm = 100.0;
        grid.margin_y_mm = 0.0;
        grid.char_size_mm = 10.0;
        grid.chars_per_line = 10;
        grid.line_gap_mm = 0.0;
        const auto pages = handwriting::layout::paginate("ab\ncd", grid);
        HR_CHECK(pages.size() == 1);
        if (pages.size() == 1) {
            HR_CHECK(pages[0].text == "ab\ncd");
            HR_CHECK(pages[0].char_offsets.size() == 4);
            HR_CHECK(pages[0].char_offsets[2] == 3);
            HR_CHECK(pages[0].char_offsets[3] == 4);
        }
    }
    {
        Settings grid;
        grid.mode = Mode::Grid;
        grid.page_height_mm = 100.0;
        grid.margin_y_mm = 0.0;
        grid.char_size_mm = 10.0;
        grid.chars_per_line = 10;
        grid.line_gap_mm = 0.0;
        const auto pages = handwriting::layout::paginate("a\n\nb", grid);
        HR_CHECK(pages.size() == 1);
        if (pages.size() == 1) {
            HR_CHECK(pages[0].text == "a\n\nb");
            HR_CHECK(pages[0].char_offsets.size() == 2);
            HR_CHECK(pages[0].char_offsets[0] == 0);
            HR_CHECK(pages[0].char_offsets[1] == 3);
        }
    }
    {
        Settings nb = notebook();
        nb.line_count = 4;
        nb.first_line = 3;
        const auto pages = handwriting::layout::paginate("a\nb\nc", nb);
        HR_CHECK(pages.size() == 2);
        if (pages.size() == 2) {
            HR_CHECK(pages[0].text == "a\nb");
            HR_CHECK(pages[1].text == "c");
        }
    }

    // ---- 参数校验 ----
    {
        Settings settings = notebook();
        settings.line_count = 0;
        bool threw = false;
        try {
            (void)handwriting::layout::compute(1, settings);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        HR_CHECK(threw);
    }

    return hrtest::summary("layout");
}
