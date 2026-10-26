// 风格模型推理 —— C++ 运行时实现（ADR-0007）
//
// Python 只负责训练并导出权重（handwriting-style-model/1 JSON）；
// 产品运行时一律走这里，不再依赖 Python。
#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "handwriting/trajectory.hpp"

namespace handwriting::style {

inline constexpr std::size_t kFeatureDim = 9;

struct Config {
    int n_points{48};
    int smooth_window{5};
    int hidden{96};
    int mixtures{5};
    double residual_scale{50.0};
    double max_residual{0.2};
};

struct Layer {
    std::size_t in{0};
    std::size_t out{0};
    std::vector<double> weight;  // row-major [out][in]
    std::vector<double> bias;    // [out]
};

class Model {
public:
    // 读取 Python 训练侧导出的 JSON 权重
    [[nodiscard]] static Model load(const std::filesystem::path& path);
    [[nodiscard]] static Model parse(const std::string& text);

    [[nodiscard]] const Config& config() const noexcept { return config_; }

    // 给一个字形/图形加风格；strength=0 返回原样，1 为完整风格
    [[nodiscard]] StrokeSet stylize(const StrokeSet& input, double strength = 1.0) const;

private:
    [[nodiscard]] std::vector<double> forward(
        const std::array<double, kFeatureDim>& features) const;

    Config config_;
    std::vector<Layer> layers_;
    Layer head_;
};

}  // namespace handwriting::style
