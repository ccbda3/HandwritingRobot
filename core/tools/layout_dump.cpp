// 版面导出工具，供 Python 侧交叉校验，防止双实现漂移。
//
// 用法：
//   hr-layout-dump <mode> <width> <height> <lines> <first_line> <char_size> <per_line> <count...>
//
// mode: notebook | grid
// count...: 每一行的字数（可给多个，代表显式换行）
//
// 输出：每行一个 cell，制表符分隔
//   x_mm  y_mm  size_mm  baseline_mm  line  column
// 最后一行输出页幅面：width_mm  height_mm  chars_per_line
//
// 为什么需要它：C++ 与 Python 各有一份版面实现（见 ADR-0003 的后果一节）。
// 两份实现漂移的话，Qt 预览与命令行导出的 G 代码就会不一致 ——
// 而这类偏差在肉眼上几乎看不出来。tests/test_layout.py 会调用本工具做比对。

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "handwriting/layout.hpp"

int main(int argc, char* argv[]) {
    if (argc < 9) {
        std::fprintf(stderr,
                     "用法: hr-layout-dump <mode> <width> <height> <lines> <first_line> "
                     "<char_size> <per_line> <count...>\n");
        return 2;
    }

    const std::string mode = argv[1];
    handwriting::layout::Settings settings;
    settings.mode = (mode == "grid") ? handwriting::layout::Mode::Grid
                                     : handwriting::layout::Mode::Notebook;
    settings.page_width_mm = std::strtod(argv[2], nullptr);
    settings.page_height_mm = std::strtod(argv[3], nullptr);
    settings.line_count = std::atoi(argv[4]);
    settings.first_line = std::atoi(argv[5]);
    settings.char_size_mm = std::strtod(argv[6], nullptr);
    settings.chars_per_line = std::atoi(argv[7]);
    settings.grid_chars_per_line = settings.chars_per_line > 0 ? settings.chars_per_line : 10;

    std::vector<std::size_t> row_counts;
    for (int i = 8; i < argc; ++i) {
        row_counts.push_back(static_cast<std::size_t>(std::atoi(argv[i])));
    }
    if (row_counts.empty()) {
        row_counts.push_back(0);
    }

    try {
        const handwriting::layout::Page page = handwriting::layout::compute(row_counts, settings);
        for (const handwriting::layout::Cell& cell : page.cells) {
            std::printf("%.9f\t%.9f\t%.9f\t%.9f\t%d\t%d\n", cell.x_mm, cell.y_mm, cell.size_mm,
                        cell.baseline_mm, cell.line, cell.column);
        }
        std::printf("PAGE\t%.9f\t%.9f\t%d\n", page.width_mm, page.height_mm, page.chars_per_line);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "版面计算失败：%s\n", error.what());
        return 1;
    }
    return 0;
}
