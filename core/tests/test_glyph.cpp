// 字形加载与坐标变换测试。与 Python 侧 tests/test_glyph.py 逐条对应。

#include <filesystem>
#include <fstream>
#include <string>

#include "check.hpp"
#include "handwriting/glyph.hpp"
#include "handwriting/json.hpp"

using handwriting::glyph::Glyph;
using handwriting::glyph::GlyphCache;
using handwriting::glyph::GlyphNotFound;

namespace {

const char* kSampleJson =
    R"({"strokes": ["M 0 0 L 10 10 Z"], "medians": [[[428, 824], [503, 781]], [[300, 300]]], "radStrokes": [0]})";

Glyph sample_glyph() {
    return handwriting::glyph::parse_glyph("永", handwriting::json::parse(kSampleJson));
}

// 临时目录，析构时清理
class TempDir {
public:
    TempDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("hr-glyph-test-" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

    void write(const std::string& filename, const std::string& content) const {
        std::ofstream stream(path_ / filename, std::ios::binary);
        stream << content;
    }

private:
    std::filesystem::path path_;
};

}  // namespace

int main() {
    // ---- 解析 ----
    {
        const Glyph glyph = sample_glyph();
        HR_CHECK(glyph.character == "永");
        HR_CHECK(glyph.stroke_count() == 2);
        HR_CHECK(glyph.point_count() == 3);
        HR_CHECK_NEAR(glyph.medians[0][0].first, 428.0);
        HR_CHECK_NEAR(glyph.medians[0][0].second, 824.0);
    }
    {
        bool threw = false;
        try {
            (void)handwriting::glyph::parse_glyph("永", handwriting::json::parse(R"({"strokes": []})"));
        } catch (const GlyphNotFound&) {
            threw = true;
        }
        HR_CHECK(threw);
    }
    {
        bool threw = false;
        try {
            (void)handwriting::glyph::parse_glyph("永", handwriting::json::parse(R"({"medians": []})"));
        } catch (const GlyphNotFound&) {
            threw = true;
        }
        HR_CHECK(threw);
    }
    {
        // 空笔画被跳过，其余保留
        const Glyph glyph =
            handwriting::glyph::parse_glyph("永", handwriting::json::parse(R"({"medians": [[], [[1, 2]]]})"));
        HR_CHECK(glyph.stroke_count() == 1);
    }

    // ---- 坐标变换 ----
    {
        // em 框左上角 → (0,0)，右下角 → (1,1)。注意 y 轴要翻转。
        Glyph glyph;
        glyph.character = "测";
        glyph.medians = {{{handwriting::glyph::kEmMinX, handwriting::glyph::kEmMaxY}},
                         {{handwriting::glyph::kEmMaxX, handwriting::glyph::kEmMinY}}};
        const handwriting::StrokeSet result = handwriting::glyph::to_stroke_set(glyph);

        HR_CHECK_NEAR(result.strokes()[0].points()[0].x, 0.0);
        HR_CHECK_NEAR(result.strokes()[0].points()[0].y, 0.0);
        HR_CHECK_NEAR(result.strokes()[1].points()[0].x, 1.0);
        HR_CHECK_NEAR(result.strokes()[1].points()[0].y, 1.0);
    }
    {
        // 原始数据 y 越大越靠上，转换后 y 越小越靠上
        Glyph glyph;
        glyph.character = "测";
        glyph.medians = {{{0.0, 800.0}}, {{0.0, 100.0}}};
        const handwriting::StrokeSet result = handwriting::glyph::to_stroke_set(glyph);
        HR_CHECK(result.strokes()[0].points()[0].y < result.strokes()[1].points()[0].y);
    }
    {
        // em 框固定，小字不会被放大到与大字一样大
        Glyph small;
        small.character = "口";
        small.medians = {{{300.0, 500.0}, {700.0, 500.0}}};
        Glyph large;
        large.character = "国";
        large.medians = {{{50.0, 800.0}, {950.0, 100.0}}};
        HR_CHECK(handwriting::glyph::to_stroke_set(small).total_length() <
                 handwriting::glyph::to_stroke_set(large).total_length() * 0.5);
    }
    {
        // 笔顺必须保留
        const handwriting::StrokeSet result = handwriting::glyph::to_stroke_set(sample_glyph());
        HR_CHECK(result.stroke_count() == 2);
        HR_CHECK_NEAR(result.strokes()[0].points()[0].x, 428.0 / handwriting::glyph::kEmSize);
        HR_CHECK_NEAR(result.strokes()[1].points()[0].x, 300.0 / handwriting::glyph::kEmSize);
    }
    {
        // 平滑会显著增加采样点
        const Glyph glyph = sample_glyph();
        HR_CHECK(handwriting::glyph::to_stroke_set(glyph, 0.01, 12, true).point_count() >
                 handwriting::glyph::to_stroke_set(glyph, 0.01, 12, false).point_count());
    }
    {
        // 基线位置：0.8789，字压横线时用它反推字格顶部
        HR_CHECK_NEAR(handwriting::glyph::kBaselineRatio, 900.0 / 1024.0);
    }

    // ---- 缓存 ----
    {
        const TempDir temp;
        const GlyphCache cache(temp.path());
        HR_CHECK(cache.path_for("永").filename().string() == "U+6C38.json");
        HR_CHECK(!cache.contains("永"));

        temp.write("U+6C38.json", kSampleJson);
        HR_CHECK(cache.contains("永"));
        HR_CHECK(cache.load("永").stroke_count() == 2);
    }
    {
        const TempDir temp;
        const GlyphCache cache(temp.path());
        bool threw = false;
        try {
            (void)cache.load("永");
        } catch (const GlyphNotFound& error) {
            threw = std::string(error.what()).find("preload") != std::string::npos;
        }
        HR_CHECK(threw);
    }
    {
        const TempDir temp;
        temp.write("U+6C38.json", "{broken");
        const GlyphCache cache(temp.path());
        bool threw = false;
        try {
            (void)cache.load("永");
        } catch (const GlyphNotFound&) {
            threw = true;
        }
        HR_CHECK(threw);
    }

    return hrtest::summary("glyph");
}
