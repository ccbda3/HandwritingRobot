#include "handwriting/curve.hpp"

#include <cmath>
#include <stdexcept>

namespace handwriting::curve {

namespace {

constexpr double kEps = 1e-12;

// 参与步进的最短线段。低于该长度的线段方向由浮点噪声主导。
constexpr double kMinSegment = 1e-6;

[[nodiscard]] double distance(const Point& a, const Point& b) noexcept {
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    return std::sqrt(dx * dx + dy * dy);
}

[[nodiscard]] Point lerp(const Point& a, const Point& b, double ratio) {
    Point out;
    out.x = a.x + (b.x - a.x) * ratio;
    out.y = a.y + (b.y - a.y) * ratio;
    out.pressure = a.pressure + (b.pressure - a.pressure) * ratio;
    out.t = static_cast<std::int32_t>(a.t + (b.t - a.t) * ratio);
    return out;
}

[[nodiscard]] Point catmull_point(const Point& p0, const Point& p1, const Point& p2,
                                  const Point& p3, double t) {
    const double t2 = t * t;
    const double t3 = t2 * t;

    const auto axis = [t, t2, t3](double a, double b, double c, double d) {
        return 0.5 * (2.0 * b + (-a + c) * t + (2.0 * a - 5.0 * b + 4.0 * c - d) * t2 +
                      (-a + 3.0 * b - 3.0 * c + d) * t3);
    };

    Point out;
    out.x = axis(p0.x, p1.x, p2.x, p3.x);
    out.y = axis(p0.y, p1.y, p2.y, p3.y);
    out.pressure = p1.pressure + (p2.pressure - p1.pressure) * t;
    out.t = static_cast<std::int32_t>(p1.t + (p2.t - p1.t) * t);
    return out;
}

}  // namespace

std::vector<Point> resample(const std::vector<Point>& points, double step) {
    if (step <= 0.0) {
        throw std::invalid_argument("step 必须为正");
    }
    if (points.size() < 2) {
        return points;
    }

    std::vector<Point> out;
    out.reserve(points.size() * 2);
    out.push_back(points.front());

    double carry = 0.0;
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const Point& a = points[i];
        const Point& b = points[i + 1];
        const double seg = distance(a, b);
        if (seg <= kEps) {
            continue;
        }

        double travelled = 0.0;
        while (carry + (seg - travelled) >= step) {
            travelled += step - carry;
            out.push_back(lerp(a, b, travelled / seg));
            carry = 0.0;
        }
        carry += seg - travelled;
    }

    const Point& last = points.back();
    if (carry > kMinSegment && (out.back().x != last.x || out.back().y != last.y)) {
        out.push_back(last);
    }
    return out;
}

std::vector<Point> catmull_rom(const std::vector<Point>& points, int samples_per_segment) {
    if (samples_per_segment < 1) {
        throw std::invalid_argument("samples_per_segment 必须 ≥ 1");
    }
    const std::size_t count = points.size();
    if (count < 2) {
        return points;
    }

    std::vector<Point> out;
    out.reserve(count * static_cast<std::size_t>(samples_per_segment) + 1);

    for (std::size_t i = 0; i + 1 < count; ++i) {
        const Point& p0 = (i > 0) ? points[i - 1] : points[i];
        const Point& p1 = points[i];
        const Point& p2 = points[i + 1];
        const Point& p3 = (i + 2 < count) ? points[i + 2] : points[i + 1];

        const bool is_last = (i + 2 == count);
        // 非末段不取 t=1，避免与下一段的起点重复；末段必须取到 t=1 才算完整
        const int steps = samples_per_segment + (is_last ? 1 : 0);
        for (int s = 0; s < steps; ++s) {
            out.push_back(catmull_point(p0, p1, p2, p3,
                                        static_cast<double>(s) /
                                            static_cast<double>(samples_per_segment)));
        }
    }
    return out;
}

std::vector<Point> smooth_and_resample(const std::vector<Point>& points, double step,
                                       int samples_per_segment) {
    if (points.size() < 2) {
        return points;
    }
    return resample(catmull_rom(points, samples_per_segment), step);
}

}  // namespace handwriting::curve
