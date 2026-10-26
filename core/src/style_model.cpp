#include "handwriting/style_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "handwriting/json.hpp"

namespace handwriting::style {
namespace {

std::vector<Point> resample_n(const std::vector<Point>& points, int n) {
    if (n < 2) {
        throw std::invalid_argument("n_points 必须 >= 2");
    }
    if (points.empty()) {
        throw std::invalid_argument("笔画不能为空");
    }
    if (points.size() == 1) {
        return std::vector<Point>(static_cast<std::size_t>(n), points.front());
    }

    std::vector<double> dist(points.size(), 0.0);
    for (std::size_t i = 1; i < points.size(); ++i) {
        dist[i] = dist[i - 1] +
                  std::hypot(points[i].x - points[i - 1].x,
                             points[i].y - points[i - 1].y);
    }
    const double total = dist.back();
    if (total <= 0.0) {
        return std::vector<Point>(static_cast<std::size_t>(n), points.front());
    }

    std::vector<Point> out;
    out.reserve(static_cast<std::size_t>(n));
    std::size_t j = 0;
    for (int i = 0; i < n; ++i) {
        const double target = total * static_cast<double>(i) / static_cast<double>(n - 1);
        while (j + 2 < points.size() && dist[j + 1] < target) {
            ++j;
        }
        const double seg = dist[j + 1] - dist[j];
        const double r = seg <= 0.0 ? 0.0 : (target - dist[j]) / seg;
        const auto& a = points[j];
        const auto& b = points[j + 1];
        out.push_back({a.x + (b.x - a.x) * r,
                       a.y + (b.y - a.y) * r,
                       a.pressure + (b.pressure - a.pressure) * r,
                       static_cast<std::int32_t>(
                           static_cast<double>(a.t) +
                           static_cast<double>(b.t - a.t) * r)});
    }
    return out;
}

std::vector<Point> smooth_points(const std::vector<Point>& points, int window) {
    if (window <= 1 || points.size() < 3) {
        return points;
    }
    const std::size_t half = static_cast<std::size_t>(window / 2);
    std::vector<Point> out(points.size());
    for (std::size_t i = 0; i < points.size(); ++i) {
        const std::size_t lo = i > half ? i - half : 0;
        const std::size_t hi = std::min(points.size(), i + half + 1);
        double sx = 0.0;
        double sy = 0.0;
        for (std::size_t j = lo; j < hi; ++j) {
            sx += points[j].x;
            sy += points[j].y;
        }
        const double count = static_cast<double>(hi - lo);
        out[i] = {sx / count, sy / count, points[i].pressure, points[i].t};
    }
    return out;
}

std::pair<double, double> tangent(const std::vector<Point>& center, std::size_t index) {
    const Point* a = nullptr;
    const Point* b = nullptr;
    if (index == 0) {
        a = &center[0];
        b = &center[1];
    } else if (index + 1 == center.size()) {
        a = &center[center.size() - 2];
        b = &center.back();
    } else {
        a = &center[index - 1];
        b = &center[index + 1];
    }
    const double tx = b->x - a->x;
    const double ty = b->y - a->y;
    const double norm = std::hypot(tx, ty);
    if (norm <= 1e-9) {
        return {0.0, 0.0};
    }
    return {tx / norm, ty / norm};
}

std::array<double, kFeatureDim> make_feature(const std::vector<Point>& center,
                                             std::size_t index,
                                             double length_norm,
                                             double prev_dx,
                                             double prev_dy,
                                             double prev_pressure) {
    const auto [tx, ty] = tangent(center, index);
    const double progress =
        center.size() > 1
            ? static_cast<double>(index) / static_cast<double>(center.size() - 1)
            : 0.0;
    return {center[index].x, center[index].y, tx, ty, progress, length_norm,
            prev_dx, prev_dy, prev_pressure};
}

double sigmoid(double x) {
    if (x >= 0.0) {
        const double e = std::exp(-x);
        return 1.0 / (1.0 + e);
    }
    const double e = std::exp(x);
    return e / (1.0 + e);
}

Layer read_layer(const json::Value& node) {
    const auto* weight = node.find("weight");
    const auto* bias = node.find("bias");
    if (weight == nullptr || bias == nullptr || !weight->is_array() ||
        !bias->is_array()) {
        throw std::runtime_error("模型层缺少 weight/bias 数组");
    }
    const auto& rows = weight->as_array();
    const auto& biases = bias->as_array();
    if (rows.empty()) {
        throw std::runtime_error("模型层 weight 为空");
    }
    if (biases.size() != rows.size()) {
        throw std::runtime_error("模型层 bias 维度与 weight 行数不一致");
    }
    const auto& first = rows.front();
    if (!first.is_array() || first.as_array().empty()) {
        throw std::runtime_error("模型层 weight 必须是二维非空数组");
    }

    Layer layer;
    layer.out = rows.size();
    layer.in = first.as_array().size();
    layer.bias.reserve(biases.size());
    for (const auto& value : biases) {
        layer.bias.push_back(value.as_number());
    }
    layer.weight.reserve(layer.out * layer.in);
    for (const auto& row : rows) {
        if (!row.is_array() || row.as_array().size() != layer.in) {
            throw std::runtime_error("模型层 weight 行长度不一致");
        }
        for (const auto& value : row.as_array()) {
            layer.weight.push_back(value.as_number());
        }
    }
    return layer;
}

}  // namespace

Model Model::load(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("无法读取模型：" + path.string());
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return parse(buffer.str());
}

Model Model::parse(const std::string& text) {
    const auto root = json::parse(text);

    const auto* format = root.find("format");
    if (format == nullptr || !format->is_string() ||
        format->as_string() != "handwriting-style-model/1") {
        throw std::runtime_error("不是 handwriting-style-model/1 权重文件");
    }

    Model model;
    if (const auto* cfg = root.find("config"); cfg != nullptr && cfg->is_object()) {
        if (const auto* v = cfg->find("n_points")) {
            model.config_.n_points = static_cast<int>(v->as_number());
        }
        if (const auto* v = cfg->find("smooth_window")) {
            model.config_.smooth_window = static_cast<int>(v->as_number());
        }
        if (const auto* v = cfg->find("hidden")) {
            model.config_.hidden = static_cast<int>(v->as_number());
        }
        if (const auto* v = cfg->find("mixtures")) {
            model.config_.mixtures = static_cast<int>(v->as_number());
        }
        if (const auto* v = cfg->find("residual_scale")) {
            model.config_.residual_scale = v->as_number();
        }
    }
    if (model.config_.n_points < 2 || model.config_.mixtures < 1 ||
        model.config_.residual_scale <= 0.0) {
        throw std::runtime_error("模型配置非法：n_points/mixtures/residual_scale");
    }

    const auto* layers = root.find("layers");
    if (layers == nullptr || !layers->is_array() || layers->as_array().empty()) {
        throw std::runtime_error("模型缺少 layers");
    }
    for (const auto& node : layers->as_array()) {
        model.layers_.push_back(read_layer(node));
    }

    const auto* head = root.find("head");
    if (head == nullptr || !head->is_object()) {
        throw std::runtime_error("模型缺少 head");
    }
    model.head_ = read_layer(*head);

    if (model.layers_.front().in != kFeatureDim) {
        throw std::runtime_error("模型输入维度必须是 9");
    }
    for (std::size_t i = 1; i < model.layers_.size(); ++i) {
        if (model.layers_[i].in != model.layers_[i - 1].out) {
            throw std::runtime_error("模型层维度不连续");
        }
    }
    if (model.head_.in != model.layers_.back().out) {
        throw std::runtime_error("head 输入维度与最后一层不一致");
    }
    const auto expected =
        static_cast<std::size_t>(5 * model.config_.mixtures + 2);
    if (model.head_.out != expected) {
        throw std::runtime_error("head 输出维度应为 5*mixtures+2");
    }
    return model;
}

std::vector<double> Model::forward(
    const std::array<double, kFeatureDim>& features) const {
    std::vector<double> hidden(features.begin(), features.end());
    for (const auto& layer : layers_) {
        std::vector<double> next(layer.out, 0.0);
        for (std::size_t o = 0; o < layer.out; ++o) {
            double sum = layer.bias[o];
            const double* row = layer.weight.data() + o * layer.in;
            for (std::size_t i = 0; i < layer.in; ++i) {
                sum += row[i] * hidden[i];
            }
            next[o] = std::tanh(sum);
        }
        hidden = std::move(next);
    }

    std::vector<double> out(head_.out, 0.0);
    for (std::size_t o = 0; o < head_.out; ++o) {
        double sum = head_.bias[o];
        const double* row = head_.weight.data() + o * head_.in;
        for (std::size_t i = 0; i < head_.in; ++i) {
            sum += row[i] * hidden[i];
        }
        out[o] = sum;
    }
    return out;
}

StrokeSet Model::stylize(const StrokeSet& input, double strength) const {
    const std::size_t n = static_cast<std::size_t>(config_.n_points);
    const std::size_t k = static_cast<std::size_t>(config_.mixtures);
    std::vector<Stroke> result;
    result.reserve(input.stroke_count());

    for (const auto& stroke : input.strokes()) {
        const auto points = resample_n(stroke.points(), config_.n_points);
        const auto center = smooth_points(points, config_.smooth_window);

        double length = 0.0;
        for (std::size_t i = 1; i < points.size(); ++i) {
            length += std::hypot(points[i].x - points[i - 1].x,
                                 points[i].y - points[i - 1].y);
        }
        const double length_norm = std::min(2.0, length);

        std::vector<Point> out;
        out.reserve(n);
        double prev_dx = 0.0;
        double prev_dy = 0.0;
        double prev_pressure = 0.5;

        for (std::size_t i = 0; i < n; ++i) {
            const auto feature = make_feature(center, i, length_norm, prev_dx,
                                              prev_dy, prev_pressure);
            const auto raw = forward(feature);

            double max_logit = raw[0];
            for (std::size_t j = 1; j < k; ++j) {
                max_logit = std::max(max_logit, raw[j]);
            }
            double denom = 0.0;
            std::vector<double> pi(k, 0.0);
            for (std::size_t j = 0; j < k; ++j) {
                pi[j] = std::exp(raw[j] - max_logit);
                denom += pi[j];
            }

            double dx_model = 0.0;
            double dy_model = 0.0;
            for (std::size_t j = 0; j < k; ++j) {
                const double w = pi[j] / denom;
                dx_model += w * raw[k + 2 * j];
                dy_model += w * raw[k + 2 * j + 1];
            }
            const double p_mu = sigmoid(raw[5 * k]);

            const double dx = std::clamp(dx_model / config_.residual_scale * strength,
                                         -config_.max_residual,
                                         config_.max_residual);
            const double dy = std::clamp(dy_model / config_.residual_scale * strength,
                                         -config_.max_residual,
                                         config_.max_residual);
            const double pressure = std::clamp(p_mu, 0.0, 1.0);

            out.push_back({center[i].x + dx, center[i].y + dy, pressure,
                           center[i].t});
            prev_dx = dx_model;
            prev_dy = dy_model;
            prev_pressure = pressure;
        }
        result.emplace_back(std::move(out));
    }

    return StrokeSet(std::move(result), input.character());
}

}  // namespace handwriting::style
