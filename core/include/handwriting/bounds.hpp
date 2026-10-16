// 工作区边界（软限位）模型。
//
// 为什么要这个模块：步进机没有位置反馈，用户上电后手推机头到物理角落、
// 再通电用点动记录四个角的坐标来标定"设备能走到哪里"。之后所有 G 代码
// 发送前都对照这个矩形做越界检查，机头就不会冲到机械端点扯断传动皮带。
//
// 本模块不认识串口硬件，也不假设坐标方向（X 右 / Y 上还是向下由示意图决定），
// 只负责"四个角 → 矩形 → 越界判定 → 持久化"。因此完全可以脱离机器测试。
#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>

namespace handwriting::bounds {

/// 标定顺序：顺时针绕一圈。
/// 用户先把机头手推到最左上角（物理起点），再点动到右 / 下 / 左三个角。
enum class Corner { TopLeft, TopRight, BottomRight, BottomLeft };

/// 矩形工作区边界（机床/工作坐标系下的轴坐标）。
struct Workspace {
    double min_x{0.0};
    double max_x{0.0};
    double min_y{0.0};
    double max_y{0.0};
    bool valid{false};

    /// 点 (x, y) 是否落在允许范围内。`margin` 为正时把允许范围
    /// 向内收缩（留余量）；为 0 时边界即物理极限。
    [[nodiscard]] bool contains(double x, double y, double margin = 0.0) const noexcept;

    /// 返回越界说明，空串表示在界内。`margin` 收缩同上。
    [[nodiscard]] std::string violation(double x, double y, double margin = 0.0) const;

    [[nodiscard]] double width() const noexcept { return max_x - min_x; }
    [[nodiscard]] double height() const noexcept { return max_y - min_y; }

    /// 四个角坐标，按 Corner 枚举顺序（约定 Y 向上：Top = max_y）。
    [[nodiscard]] std::array<std::pair<double, double>, 4> corners() const noexcept;
};

/// 从四个角（按 TopLeft→TopRight→BottomRight→BottomLeft 顺序）构造矩形。
/// 自动取 min/max 归一化，所以无论用户记录的物理方向如何都能得到正确矩形。
/// 至少需要两个不同点才能构成有效矩形。
[[nodiscard]] Workspace from_corners(const std::array<std::pair<double, double>, 4>& points);

/// 从程序运动范围构造工作区（左上角对齐到原点）。用于"直接用 G 代码范围当边界"的简化场景。
[[nodiscard]] Workspace from_program_bounds(double min_x, double min_y, double max_x,
                                           double max_y);

/// 持久化到 JSON。文件不存在或损坏时返回 invalid。
[[nodiscard]] Workspace load(const std::filesystem::path& path);

/// 写入 JSON（覆盖）。
void save(const Workspace& ws, const std::filesystem::path& path);

/// 默认保存路径：仓库内 data/workspace-bounds.json。
[[nodiscard]] std::filesystem::path default_path();

}  // namespace handwriting::bounds
