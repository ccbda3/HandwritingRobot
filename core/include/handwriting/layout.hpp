// 版面计算 —— 字格从哪来
//
// 两种模式：
//
// ``Grid``
//     通用排版：固定字格 + 字距 + 行距 + 页边距。用于导到空白纸。
//
// ``Notebook``
//     横条本（作业本）：字格按页宽与行数均分，**字压在横线上**。
//     默认幅面 150×213 mm / 22 行 —— 常见作业本去掉页边距后的书写区。
//
// 关键点：字形在字格内的位置由**基线**决定，不是由字格顶部决定。
// 字形数据里基线固定在归一化坐标的 0.8789 处（见 glyph.hpp 的 kBaselineRatio），
// 因此「字压在横线上」要反推字格顶部：top = baseline − 0.8789 × char_size。
// 如果按字格顶部对齐，字会整体浮在横线上方约 12% 字高的位置。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace handwriting::layout {

enum class Mode { Grid, Notebook };

struct Settings {
    Mode mode{Mode::Notebook};

    // ---- 幅面（mm）----
    double page_width_mm{150.0};
    double page_height_mm{213.0};

    // ---- 字格 ----
    // 0 表示自动：Notebook 模式取行距，Grid 模式取 20mm
    double char_size_mm{0.0};
    // 0 表示自动：按页宽和字宽算出能放几个
    int chars_per_line{0};

    // ---- Notebook 模式 ----
    int line_count{22};
    // 从第几行开始写（1 起）。留白或避开本子上的印刷标题时用。
    int first_line{1};

    // ---- Grid 模式 ----
    double char_gap_mm{3.0};
    double line_gap_mm{10.0};
    double margin_x_mm{10.0};
    double margin_y_mm{10.0};
    int grid_chars_per_line{10};
};

struct Cell {
    double x_mm{0.0};         // 字格左上角
    double y_mm{0.0};
    double size_mm{0.0};      // 字格边长
    double baseline_mm{0.0};  // 基线 y。字压在这条线上。
    int line{0};              // 行号（0 起，从页顶算）
    int column{0};            // 列号（0 起）
};

struct Page {
    double width_mm{0.0};
    double height_mm{0.0};
    int chars_per_line{1};
    std::vector<Cell> cells;              // 按书写顺序（行优先）
    std::vector<double> rule_lines_mm;    // 横线的 y 坐标，仅用于绘制预览
};

// 解析后的实际字宽与每行字数。UI 需要显示这两个值。
[[nodiscard]] double resolve_char_size(const Settings& settings);
[[nodiscard]] int resolve_chars_per_line(const Settings& settings);

// 计算版面。
//
// ``row_char_counts`` 为每一行的字数（文本里的显式换行产生新行）。
// 超出 ``chars_per_line`` 的行会自动折到下一行。
[[nodiscard]] Page compute(const std::vector<std::size_t>& row_char_counts,
                           const Settings& settings);

// 便捷重载：单个连续段落
[[nodiscard]] Page compute(std::size_t char_count, const Settings& settings);

// 整行风格：在版面基础上加入「字大小 / 字距 / 基线」的统计波动。
//
// 这些量来自真实整行采集（LineStyleFeatures 的 char_width_cv / char_gap_cv /
// baseline_drift / baseline_slope）。全为 0 时退化为工整版面。
struct StyleProfile {
    double size_cv{0.0};            // 字宽变异系数（相对标准差）
    double gap_mean_mm{3.0};        // Grid 模式的平均字距
    double gap_cv{0.0};             // 字距变异系数
    double baseline_drift_mm{0.0};  // 基线上下抖动（标准差）
    double baseline_slope_mm{0.0};  // 基线斜率（每列 mm）
    std::uint32_t seed{20261026};
};

[[nodiscard]] Page compute_styled(const std::vector<std::size_t>& row_char_counts,
                                  const Settings& settings,
                                  const StyleProfile& profile);

}  // namespace handwriting::layout
