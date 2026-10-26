// 把 C++ 推理结果按 CSV 打印，供 Python 侧做跨语言对拍。
//
// 用法：hr-style-dump <model.json>
#include <cstdio>
#include <vector>

#include "handwriting/style_model.hpp"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: hr-style-dump <model.json>\n");
        return 2;
    }

    const auto model = handwriting::style::Model::load(argv[1]);
    const std::vector<handwriting::Point> points = {
        {0.0, 0.0, 0.5, 0}, {1.0, 0.0, 0.5, 10}};
    const handwriting::StrokeSet input({handwriting::Stroke(points)}, "一");
    const auto output = model.stylize(input);

    for (const auto& point : output.strokes()[0].points()) {
        std::printf("%.9f,%.9f,%.9f\n", point.x, point.y, point.pressure);
    }
    return 0;
}
