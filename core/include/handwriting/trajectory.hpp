// 轨迹数据结构 —— C++ 核心库
//
// 这是生产路径的唯一实现。Python 侧（src/handwriting/）保留一份等价实现，
// 用于训练期的快速实验与参数搜索；两者的一致性由 tests/parity 交叉校验。
//
// 坐标约定与采集器一致：x 向右为正，y 向下为正，取值 0~1 为归一化到方格内部。
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace handwriting {

struct Point {
    double x{0.0};
    double y{0.0};
    double pressure{0.5};
    std::int32_t t{0};
};

struct Bounds {
    double min_x{0.0};
    double min_y{0.0};
    double max_x{0.0};
    double max_y{0.0};

    [[nodiscard]] double width() const noexcept { return max_x - min_x; }
    [[nodiscard]] double height() const noexcept { return max_y - min_y; }
    [[nodiscard]] double longest_side() const noexcept;
};

class Stroke {
public:
    Stroke() = default;
    explicit Stroke(std::vector<Point> points);

    [[nodiscard]] const std::vector<Point>& points() const noexcept { return points_; }
    [[nodiscard]] bool empty() const noexcept { return points_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return points_.size(); }

    [[nodiscard]] double length() const noexcept;
    [[nodiscard]] std::int32_t duration_ms() const noexcept;
    [[nodiscard]] double mean_pressure() const noexcept;
    [[nodiscard]] std::optional<Bounds> bounds() const noexcept;

    [[nodiscard]] Stroke translated(double dx, double dy) const;
    [[nodiscard]] Stroke sheared(double kx, double ky) const;

private:
    std::vector<Point> points_;
};

class StrokeSet {
public:
    StrokeSet() = default;
    explicit StrokeSet(std::vector<Stroke> strokes, std::string character = {});

    [[nodiscard]] const std::vector<Stroke>& strokes() const noexcept { return strokes_; }
    [[nodiscard]] const std::string& character() const noexcept { return character_; }
    [[nodiscard]] std::size_t stroke_count() const noexcept { return strokes_.size(); }
    [[nodiscard]] std::size_t point_count() const noexcept;
    [[nodiscard]] double total_length() const noexcept;
    [[nodiscard]] std::optional<Bounds> bounds() const noexcept;

    // 等比缩放并居中到 size x size 的方框，保留字形宽高比 —— 拉伸会破坏风格特征。
    [[nodiscard]] StrokeSet fit_to_box(double size = 1.0, double padding = 0.05) const;

private:
    std::vector<Stroke> strokes_;
    std::string character_;
};

}  // namespace handwriting
