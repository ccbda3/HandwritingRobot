// C++ 风格模型推理的最小回归测试。
//
// 用一个全零权重的微型模型：输出残差恒为 0、笔压恒为 0.5，
// 因此 stylize 的结果应当等于「重采样后的中心线」，可以直接断言坐标。
#include "handwriting/style_model.hpp"

#include <string>
#include <vector>

#include "check.hpp"

int main() {
    const std::string tiny = R"({
      "format": "handwriting-style-model/1",
      "config": {
        "n_points": 4,
        "smooth_window": 1,
        "hidden": 1,
        "mixtures": 1,
        "residual_scale": 50.0
      },
      "layers": [
        {"weight": [[0,0,0,0,0,0,0,0,0]], "bias": [0]},
        {"weight": [[0]], "bias": [0]}
      ],
      "head": {"weight": [[0],[0],[0],[0],[0],[0],[0]], "bias": [0,0,0,0,0,0,0]}
    })";

    const auto model = handwriting::style::Model::parse(tiny);
    HR_CHECK(model.config().n_points == 4);
    HR_CHECK(model.config().mixtures == 1);

    const std::vector<handwriting::Point> points = {
        {0.0, 0.0, 0.5, 0}, {1.0, 0.0, 0.5, 10}};
    const handwriting::StrokeSet input({handwriting::Stroke(points)}, "一");
    const auto output = model.stylize(input);

    HR_CHECK(output.stroke_count() == 1);
    HR_CHECK(output.strokes()[0].size() == 4);
    const auto& out = output.strokes()[0].points();
    HR_CHECK_NEAR(out[0].x, 0.0);
    HR_CHECK_NEAR(out[1].x, 1.0 / 3.0);
    HR_CHECK_NEAR(out[2].x, 2.0 / 3.0);
    HR_CHECK_NEAR(out[3].x, 1.0);
    for (const auto& point : out) {
        HR_CHECK_NEAR(point.y, 0.0);
        HR_CHECK_NEAR(point.pressure, 0.5);
    }

    // strength=0 时也必须保持原中心线
    const auto plain = model.stylize(input, 0.0);
    HR_CHECK_NEAR(plain.strokes()[0].points()[3].x, 1.0);

    return hrtest::summary("style_model");
}
