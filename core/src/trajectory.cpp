#include "handwriting/trajectory.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace handwriting {

namespace {
constexpr std::size_t kMinPointsPerStroke = 1;
}  // namespace

double Bounds::longest_side() const noexcept {
    return std::max(width(), height());
}

Stroke::Stroke(std::vector<Point> points) : points_(std::move(points)) {
    if (points_.size() < kMinPointsPerStroke) {
        throw std::invalid_argument("Stroke 至少需要一个采样点");
    }
}

double Stroke::length() const noexcept {
    double total = 0.0;
    for (std::size_t i = 1; i < points_.size(); ++i) {
        const double dx = points_[i].x - points_[i - 1].x;
        const double dy = points_[i].y - points_[i - 1].y;
        total += std::sqrt(dx * dx + dy * dy);
    }
    return total;
}

std::int32_t Stroke::duration_ms() const noexcept {
    if (points_.empty()) {
        return 0;
    }
    return points_.back().t - points_.front().t;
}

double Stroke::mean_pressure() const noexcept {
    if (points_.empty()) {
        return 0.0;
    }
    double total = 0.0;
    for (const auto& point : points_) {
        total += point.pressure;
    }
    return total / static_cast<double>(points_.size());
}

std::optional<Bounds> Stroke::bounds() const noexcept {
    if (points_.empty()) {
        return std::nullopt;
    }
    Bounds box{points_.front().x, points_.front().y, points_.front().x, points_.front().y};
    for (const auto& point : points_) {
        box.min_x = std::min(box.min_x, point.x);
        box.min_y = std::min(box.min_y, point.y);
        box.max_x = std::max(box.max_x, point.x);
        box.max_y = std::max(box.max_y, point.y);
    }
    return box;
}

Stroke Stroke::translated(double dx, double dy) const {
    std::vector<Point> moved = points_;
    for (auto& point : moved) {
        point.x += dx;
        point.y += dy;
    }
    return Stroke(std::move(moved));
}

Stroke Stroke::sheared(double kx, double ky) const {
    std::vector<Point> skewed = points_;
    for (auto& point : skewed) {
        const double x = point.x + kx * point.y;
        const double y = point.y + ky * point.x;
        point.x = x;
        point.y = y;
    }
    return Stroke(std::move(skewed));
}

StrokeSet::StrokeSet(std::vector<Stroke> strokes, std::string character)
    : strokes_(std::move(strokes)), character_(std::move(character)) {}

std::size_t StrokeSet::point_count() const noexcept {
    std::size_t total = 0;
    for (const auto& stroke : strokes_) {
        total += stroke.size();
    }
    return total;
}

double StrokeSet::total_length() const noexcept {
    double total = 0.0;
    for (const auto& stroke : strokes_) {
        total += stroke.length();
    }
    return total;
}

std::optional<Bounds> StrokeSet::bounds() const noexcept {
    std::optional<Bounds> result;
    for (const auto& stroke : strokes_) {
        const auto box = stroke.bounds();
        if (!box.has_value()) {
            continue;
        }
        if (!result.has_value()) {
            result = box;
            continue;
        }
        result->min_x = std::min(result->min_x, box->min_x);
        result->min_y = std::min(result->min_y, box->min_y);
        result->max_x = std::max(result->max_x, box->max_x);
        result->max_y = std::max(result->max_y, box->max_y);
    }
    return result;
}

StrokeSet StrokeSet::fit_to_box(double size, double padding) const {
    const auto box = bounds();
    if (!box.has_value()) {
        return StrokeSet{};
    }

    const double longest = box->longest_side();
    if (longest <= 0.0) {
        return StrokeSet{};
    }

    const double target = size * (1.0 - 2.0 * padding);
    const double factor = target / longest;
    const double offset_x = -box->min_x * factor + (size - box->width() * factor) / 2.0;
    const double offset_y = -box->min_y * factor + (size - box->height() * factor) / 2.0;

    std::vector<Stroke> fitted;
    fitted.reserve(strokes_.size());
    for (const auto& stroke : strokes_) {
        std::vector<Point> points = stroke.points();
        for (auto& point : points) {
            point.x = point.x * factor + offset_x;
            point.y = point.y * factor + offset_y;
        }
        fitted.emplace_back(std::move(points));
    }
    return StrokeSet(std::move(fitted), character_);
}

}  // namespace handwriting
