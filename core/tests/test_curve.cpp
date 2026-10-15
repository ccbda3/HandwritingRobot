// 曲线插值与重采样测试。与 Python 侧 tests/test_curve.py 逐条对应。

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <stdexcept>
#include <utility>
#include <vector>

#include "check.hpp"
#include "handwriting/curve.hpp"
#include "handwriting/trajectory.hpp"

using handwriting::Point;

namespace {

std::vector<Point> polyline(std::initializer_list<std::pair<double, double>> coords) {
    std::vector<Point> points;
    for (const auto& [x, y] : coords) {
        Point point;
        point.x = x;
        point.y = y;
        point.pressure = 0.5;
        point.t = 0;
        points.push_back(point);
    }
    return points;
}

double distance(const Point& a, const Point& b) {
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    return std::sqrt(dx * dx + dy * dy);
}

double max_turn_deg(const std::vector<Point>& points) {
    double worst = 0.0;
    for (std::size_t i = 1; i + 1 < points.size(); ++i) {
        const double ax = points[i].x - points[i - 1].x;
        const double ay = points[i].y - points[i - 1].y;
        const double bx = points[i + 1].x - points[i].x;
        const double by = points[i + 1].y - points[i].y;
        const double na = std::sqrt(ax * ax + ay * ay);
        const double nb = std::sqrt(bx * bx + by * by);
        if (na < 1e-9 || nb < 1e-9) {
            continue;
        }
        double cosine = (ax * bx + ay * by) / (na * nb);
        cosine = std::min(1.0, std::max(-1.0, cosine));
        worst = std::max(worst, std::acos(cosine) * 180.0 / 3.14159265358979323846);
    }
    return worst;
}

}  // namespace

int main() {
    // ---- 重采样 ----
    {
        const auto result = handwriting::curve::resample(polyline({{0.0, 0.0}, {1.0, 0.0}}), 0.05);
        HR_CHECK(result.size() >= 20 && result.size() <= 22);
        HR_CHECK_NEAR(result.front().x, 0.0);
        HR_CHECK_NEAR(result.back().x, 1.0);
        for (std::size_t i = 1; i < result.size(); ++i) {
            HR_CHECK(distance(result[i - 1], result[i]) <= 0.05 + 1e-9);
        }
    }
    {
        // L 形：横 0.5 + 竖 0.5 = 1.0，步长 0.1 → 约 11 个点
        const auto result =
            handwriting::curve::resample(polyline({{0.0, 0.0}, {0.5, 0.0}, {0.5, 0.5}}), 0.1);
        HR_CHECK(result.size() >= 10 && result.size() <= 12);
    }
    {
        const auto result = handwriting::curve::resample(polyline({{0.0, 0.0}, {0.01, 0.0}}), 0.1);
        HR_CHECK(result.size() == 2);
    }
    {
        // 退化线段（重复点）不应导致除零或死循环
        const auto result =
            handwriting::curve::resample(polyline({{0.0, 0.0}, {0.0, 0.0}, {0.5, 0.0}}), 0.1);
        HR_CHECK_NEAR(result.front().x, 0.0);
        HR_CHECK_NEAR(result.back().x, 0.5);
    }
    {
        const std::vector<Point> single = polyline({{0.5, 0.5}});
        HR_CHECK(handwriting::curve::resample(single, 0.1).size() == 1);
    }
    {
        bool threw = false;
        try {
            (void)handwriting::curve::resample(polyline({{0.0, 0.0}, {1.0, 0.0}}), 0.0);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        HR_CHECK(threw);
    }
    {
        // 压力与时间应被插值
        std::vector<Point> ends = polyline({{0.0, 0.0}, {1.0, 0.0}});
        ends[0].pressure = 0.0;
        ends[1].pressure = 1.0;
        ends[1].t = 100;
        const auto result = handwriting::curve::resample(ends, 0.25);
        const Point& middle = result[result.size() / 2];
        HR_CHECK(middle.pressure > 0.0 && middle.pressure < 1.0);
        HR_CHECK(middle.t > 0 && middle.t < 100);
    }

    // ---- Catmull-Rom ----
    {
        // 关键性质：曲线通过每一个控制点
        const auto control = polyline({{0.0, 0.0}, {0.3, 0.4}, {0.6, 0.2}, {1.0, 0.8}});
        const auto result = handwriting::curve::catmull_rom(control, 8);
        for (std::size_t i = 0; i < control.size(); ++i) {
            const Point& got = result[i * 8];
            HR_CHECK_NEAR(got.x, control[i].x);
            HR_CHECK_NEAR(got.y, control[i].y);
        }
    }
    {
        // n 个控制点 → (n-1) 段；非末段取 samples 个，末段多取一个收尾点
        const auto result = handwriting::curve::catmull_rom(polyline({{0.0, 0.0}, {0.5, 0.0}, {1.0, 0.0}}), 4);
        HR_CHECK(result.size() == 9);
    }
    {
        // 端点不外扩
        const auto result = handwriting::curve::catmull_rom(polyline({{0.0, 0.0}, {0.5, 0.0}, {1.0, 0.0}}), 10);
        double min_x = 1e9;
        double max_x = -1e9;
        for (const Point& point : result) {
            min_x = std::min(min_x, point.x);
            max_x = std::max(max_x, point.x);
        }
        HR_CHECK_NEAR(min_x, 0.0);
        HR_CHECK_NEAR(max_x, 1.0);
    }
    {
        // 共线点应保持共线
        const auto result =
            handwriting::curve::catmull_rom(polyline({{0.0, 0.5}, {0.25, 0.5}, {0.5, 0.5}, {1.0, 0.5}}), 6);
        for (const Point& point : result) {
            HR_CHECK_NEAR(point.y, 0.5);
        }
    }
    {
        // 样条应当把折角摊开
        const auto control = polyline({{0.0, 0.0}, {0.5, 0.0}, {0.5, 0.5}});
        const auto smoothed = handwriting::curve::catmull_rom(control, 20);
        HR_CHECK(max_turn_deg(smoothed) < max_turn_deg(control) - 30.0);
    }
    {
        bool threw = false;
        try {
            (void)handwriting::curve::catmull_rom(polyline({{0.0, 0.0}, {1.0, 0.0}}), 0);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        HR_CHECK(threw);
    }

    // ---- 组合：平滑顺序不能颠倒 ----
    {
        const auto result = handwriting::curve::smooth_and_resample(
            polyline({{0.0, 0.0}, {0.5, 0.0}, {0.5, 0.5}}), 0.05);
        // 若先重采样后平滑，L 形拐角会保留接近 90° 的突变
        HR_CHECK(max_turn_deg(result) < 45.0);
    }
    {
        // 重采样保证的是弧长均匀，不是弦长均匀：弦长一律不超过步长
        const auto result = handwriting::curve::smooth_and_resample(
            polyline({{0.0, 0.0}, {0.3, 0.4}, {0.6, 0.2}, {1.0, 0.8}}), 0.02);
        for (std::size_t i = 1; i < result.size(); ++i) {
            HR_CHECK(distance(result[i - 1], result[i]) <= 0.02 + 1e-9);
        }
    }

    return hrtest::summary("curve");
}
