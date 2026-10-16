#include "handwriting/bounds.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "handwriting/json.hpp"

namespace handwriting::bounds {

bool Workspace::contains(double x, double y, double margin) const noexcept {
    if (!valid) {
        return false;
    }
    const double lo_x = min_x + margin;
    const double hi_x = max_x - margin;
    const double lo_y = min_y + margin;
    const double hi_y = max_y - margin;
    if (lo_x > hi_x || lo_y > hi_y) {
        // 收缩过头（margin 太大），此时任何点都不算"安全"
        return false;
    }
    return x >= lo_x && x <= hi_x && y >= lo_y && y <= hi_y;
}

std::string Workspace::violation(double x, double y, double margin) const {
    if (!valid) {
        return "工作区未标定";
    }
    const double lo_x = min_x + margin;
    const double hi_x = max_x - margin;
    const double lo_y = min_y + margin;
    const double hi_y = max_y - margin;

    std::vector<std::string> parts;
    if (x < lo_x) {
        parts.push_back("X 超出左边界 " + std::to_string(lo_x) + "mm");
    } else if (x > hi_x) {
        parts.push_back("X 超出右边界 " + std::to_string(hi_x) + "mm");
    }
    if (y < lo_y) {
        parts.push_back("Y 超出下边界 " + std::to_string(lo_y) + "mm");
    } else if (y > hi_y) {
        parts.push_back("Y 超出上边界 " + std::to_string(hi_y) + "mm");
    }
    std::string reason;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) {
            reason += " ; ";
        }
        reason += parts[i];
    }
    return reason;
}

std::array<std::pair<double, double>, 4> Workspace::corners() const noexcept {
    return {std::make_pair(min_x, max_y), std::make_pair(max_x, max_y),
            std::make_pair(max_x, min_y), std::make_pair(min_x, min_y)};
}

Workspace from_corners(const std::array<std::pair<double, double>, 4>& points) {
    Workspace ws;
    double min_x = points[0].first;
    double max_x = points[0].first;
    double min_y = points[0].second;
    double max_y = points[0].second;
    for (std::size_t i = 1; i < points.size(); ++i) {
        min_x = std::min(min_x, points[i].first);
        max_x = std::max(max_x, points[i].first);
        min_y = std::min(min_y, points[i].second);
        max_y = std::max(max_y, points[i].second);
    }
    ws.min_x = min_x;
    ws.max_x = max_x;
    ws.min_y = min_y;
    ws.max_y = max_y;
    // 至少需要某个轴上有跨度才视为有效矩形
    ws.valid = (max_x - min_x) > 1e-6 && (max_y - min_y) > 1e-6;
    return ws;
}

Workspace from_program_bounds(double min_x, double min_y, double max_x, double max_y) {
    Workspace ws;
    ws.min_x = std::min(min_x, max_x);
    ws.max_x = std::max(min_x, max_x);
    ws.min_y = std::min(min_y, max_y);
    ws.max_y = std::max(min_y, max_y);
    ws.valid = (ws.max_x - ws.min_x) > 1e-6 && (ws.max_y - ws.min_y) > 1e-6;
    return ws;
}

Workspace load(const std::filesystem::path& path) {
    Workspace ws;
    try {
        const auto value = json::parse_file(path);
        if (!value.is_object()) {
            return ws;
        }
        const auto* vmin_x = value.find("min_x");
        const auto* vmax_x = value.find("max_x");
        const auto* vmin_y = value.find("min_y");
        const auto* vmax_y = value.find("max_y");
        const auto* vvalid = value.find("valid");
        if (vmin_x == nullptr || vmax_x == nullptr || vmin_y == nullptr || vmax_y == nullptr) {
            return ws;
        }
        ws.min_x = vmin_x->as_number();
        ws.max_x = vmax_x->as_number();
        ws.min_y = vmin_y->as_number();
        ws.max_y = vmax_y->as_number();
        ws.valid = vvalid ? vvalid->as_bool() : true;
    } catch (const json::ParseError&) {
        return ws;
    }
    return ws;
}

void save(const Workspace& ws, const std::filesystem::path& path) {
    char buffer[256];
    const int written = std::snprintf(
        buffer, sizeof(buffer),
        "{\n"
        "  \"min_x\": %.4f,\n"
        "  \"max_x\": %.4f,\n"
        "  \"min_y\": %.4f,\n"
        "  \"max_y\": %.4f,\n"
        "  \"valid\": %s\n"
        "}\n",
        ws.min_x, ws.max_x, ws.min_y, ws.max_y, ws.valid ? "true" : "false");

    std::string text;
    if (written >= 0 && static_cast<std::size_t>(written) < sizeof(buffer)) {
        text.assign(buffer, static_cast<std::size_t>(written));
    } else {
        // 数据过长（理论不会发生），用保守写法兜底
        text = "{\"min_x\":" + std::to_string(ws.min_x) + ",\"max_x\":" + std::to_string(ws.max_x) +
               ",\"min_y\":" + std::to_string(ws.min_y) + ",\"max_y\":" + std::to_string(ws.max_y) +
               ",\"valid\":" + (ws.valid ? "true" : "false") + "}";
    }

    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    FILE* file = std::fopen(path.string().c_str(), "wb");
    if (file == nullptr) {
        return;  // 保存失败静默返回，标定数据仍在内存里
    }
    std::fwrite(text.data(), 1, text.size(), file);
    std::fclose(file);
}

std::filesystem::path default_path() {
    return std::filesystem::path("data") / "workspace-bounds.json";
}

}  // namespace handwriting::bounds
