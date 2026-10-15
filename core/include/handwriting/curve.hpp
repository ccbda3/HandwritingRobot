// 曲线插值与重采样 —— C++ 核心库
//
// 与 Python 侧 src/handwriting/curve.py 行为等价。两侧必须同步修改，
// 否则 Qt 预览看到的字形与命令行导出的 G 代码会不一致。
#pragma once

#include <cstddef>
#include <vector>

#include "handwriting/trajectory.hpp"

namespace handwriting::curve {

// 按固定弧长重采样，压力与时间线性插值。
//
// 末段短于内部阈值时不再补端点：浮点累积（如 25 × 0.02 ≠ 0.5）会产生
// 长度接近 0 的末段，其方向由噪声主导，会让下游的转折角计算完全错误。
[[nodiscard]] std::vector<Point> resample(const std::vector<Point>& points, double step);

// Catmull-Rom 样条插值，把稀疏折线变成平滑曲线。
//
// 选它而不是 B 样条：它**通过所有控制点**。字形中心线的控制点本身
// 落在笔画上，被平滑掉就意味着笔画走偏。
// 端点采用重复法处理（首段令 P0 = P1），曲线端点因此不会外扩。
[[nodiscard]] std::vector<Point> catmull_rom(const std::vector<Point>& points,
                                             int samples_per_segment = 12);

// 先样条插值再按弧长重采样。顺序不能颠倒：先重采样会让稀疏控制点之间的
// 直线段被固化，样条插值再平滑也救不回来。
[[nodiscard]] std::vector<Point> smooth_and_resample(const std::vector<Point>& points,
                                                     double step,
                                                     int samples_per_segment = 12);

}  // namespace handwriting::curve
