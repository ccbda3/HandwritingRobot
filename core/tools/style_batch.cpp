// 批量风格推理：stdin 读 JSON，stdout 写 JSON。
//
// 用法：hr-style-batch --model style-model.json < input.json > output.json
//
// 输入：{"strength":1.0,"sets":[{"char":"永","strokes":[[[x,y,p,t],...],...]}, ...]}
// 输出：{"sets":[{"char":"永","strokes":[[[x,y,p,t],...],...]}, ...]}
//
// 开发服务器用它代替 Python 推理，保证运行时只有 C++（ADR-0007）。
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "handwriting/json.hpp"
#include "handwriting/style_model.hpp"

namespace {

std::string escape_json(const std::string& text) {
    std::string out;
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    return out;
}

std::vector<handwriting::Point> parse_points(const handwriting::json::Value& node) {
    if (!node.is_array()) {
        throw std::runtime_error("stroke 必须是点数组");
    }
    std::vector<handwriting::Point> points;
    for (const auto& item : node.as_array()) {
        const auto& values = item.as_array();
        if (values.size() < 4) {
            throw std::runtime_error("点必须是 [x,y,pressure,t]");
        }
        points.push_back({values[0].as_number(), values[1].as_number(),
                          values[2].as_number(),
                          static_cast<std::int32_t>(values[3].as_number())});
    }
    return points;
}

}  // namespace

int main(int argc, char** argv) {
    std::string model_path;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--model" && i + 1 < argc) {
            model_path = argv[++i];
        }
    }
    if (model_path.empty()) {
        std::fprintf(stderr, "usage: hr-style-batch --model <style-model.json>\n");
        return 2;
    }

    try {
        const auto model = handwriting::style::Model::load(model_path);

        std::ostringstream buffer;
        buffer << std::cin.rdbuf();
        const auto root = handwriting::json::parse(buffer.str());

        double strength = 1.0;
        if (const auto* value = root.find("strength")) {
            strength = value->as_number();
        }
        const auto* sets = root.find("sets");
        if (sets == nullptr || !sets->is_array()) {
            throw std::runtime_error("输入缺少 sets 数组");
        }

        std::ostringstream out;
        out << std::setprecision(9);
        out << "{\"sets\":[";
        bool first_set = true;
        for (const auto& set_node : sets->as_array()) {
            const auto* strokes_node = set_node.find("strokes");
            if (strokes_node == nullptr || !strokes_node->is_array()) {
                throw std::runtime_error("set 缺少 strokes 数组");
            }
            std::vector<handwriting::Stroke> strokes;
            for (const auto& stroke_node : strokes_node->as_array()) {
                strokes.emplace_back(parse_points(stroke_node));
            }
            std::string character;
            if (const auto* value = set_node.find("char")) {
                character = value->as_string();
            }

            const auto styled = model.stylize(
                handwriting::StrokeSet(std::move(strokes), character), strength);

            if (!first_set) {
                out << ",";
            }
            first_set = false;
            out << "{\"char\":\"" << escape_json(styled.character())
                << "\",\"strokes\":[";
            bool first_stroke = true;
            for (const auto& stroke : styled.strokes()) {
                if (!first_stroke) {
                    out << ",";
                }
                first_stroke = false;
                out << "[";
                bool first_point = true;
                for (const auto& point : stroke.points()) {
                    if (!first_point) {
                        out << ",";
                    }
                    first_point = false;
                    out << "[" << point.x << "," << point.y << ","
                        << point.pressure << "," << point.t << "]";
                }
                out << "]";
            }
            out << "]}";
        }
        out << "]}";
        std::cout << out.str();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "hr-style-batch: %s\n", error.what());
        return 1;
    }
}
