#include "handwriting/glyph.hpp"

#include <fstream>
#include <sstream>

#include "handwriting/curve.hpp"
#include "handwriting/utf8.hpp"

namespace handwriting::glyph {

namespace {

[[nodiscard]] std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw GlyphNotFound("无法读取字形文件：" + path.string());
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

}  // namespace

std::size_t Glyph::point_count() const noexcept {
    std::size_t total = 0;
    for (const auto& median : medians) {
        total += median.size();
    }
    return total;
}

Glyph parse_glyph(const std::string& character, const json::Value& payload) {
    const json::Value* medians = payload.find("medians");
    if (medians == nullptr || !medians->is_array()) {
        throw GlyphNotFound(character + " 的字形数据缺少 medians 字段");
    }

    Glyph out;
    out.character = character;

    for (const json::Value& median : medians->as_array()) {
        if (!median.is_array()) {
            continue;
        }
        std::vector<std::pair<double, double>> points;
        const json::Array& raw = median.as_array();
        points.reserve(raw.size());
        for (const json::Value& item : raw) {
            if (!item.is_array() || item.as_array().size() < 2) {
                continue;
            }
            points.emplace_back(item.at(0).as_number(), item.at(1).as_number());
        }
        if (!points.empty()) {
            out.medians.push_back(std::move(points));
        }
    }

    if (out.medians.empty()) {
        throw GlyphNotFound(character + " 没有任何有效的笔画中心线");
    }
    return out;
}

GlyphCache::GlyphCache(std::filesystem::path directory) : directory_(std::move(directory)) {}

std::filesystem::path GlyphCache::path_for(const std::string& character) const {
    if (character.empty()) {
        throw GlyphNotFound("字符为空，无法定位字形文件");
    }
    return directory_ / (utf8::codepoint_tag(utf8::first_codepoint(character)) + ".json");
}

bool GlyphCache::contains(const std::string& character) const {
    std::error_code error;
    return std::filesystem::is_regular_file(path_for(character), error);
}

Glyph GlyphCache::load(const std::string& character) const {
    const std::filesystem::path path = path_for(character);
    if (!std::filesystem::exists(path)) {
        throw GlyphNotFound(character + " 的字形不在本地缓存中（" + path.string() +
                            "）。请先用 Python 侧执行：handwriting preload");
    }
    try {
        return parse_glyph(character, json::parse(read_text_file(path)));
    } catch (const json::ParseError& error) {
        throw GlyphNotFound(character + " 的字形文件损坏：" + error.what());
    }
}

StrokeSet to_stroke_set(const Glyph& glyph, double step, int samples_per_segment, bool smooth) {
    std::vector<Stroke> strokes;
    strokes.reserve(glyph.medians.size());

    for (const auto& median : glyph.medians) {
        if (median.empty()) {
            continue;
        }
        std::vector<Point> points;
        points.reserve(median.size());
        for (const auto& [x, y] : median) {
            Point point;
            point.x = (x - kEmMinX) / kEmSize;
            point.y = (kEmMaxY - y) / kEmSize;  // y 轴翻转
            point.pressure = 0.5;
            point.t = 0;
            points.push_back(point);
        }
        if (smooth && points.size() >= 2) {
            points = curve::smooth_and_resample(points, step, samples_per_segment);
        }
        strokes.emplace_back(std::move(points));
    }
    return StrokeSet(std::move(strokes), glyph.character);
}

}  // namespace handwriting::glyph
