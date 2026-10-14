// 轨迹数据结构测试。与 Python 侧 tests/test_stroke.py 逐条对应，
// 任何一侧行为变化都必须同步，否则 parity 检查会失败。

#include "check.hpp"
#include "handwriting/trajectory.hpp"

#include <stdexcept>
#include <vector>

using handwriting::Point;
using handwriting::Stroke;
using handwriting::StrokeSet;

namespace {

Stroke diagonal_stroke() {
    return Stroke({Point{0.0, 0.0}, Point{0.3, 0.4}, Point{0.3, 0.8}});
}

}  // namespace

int main() {
    // ---- Stroke 基础量 ----
    HR_CHECK_NEAR(diagonal_stroke().length(), 0.9);
    HR_CHECK(diagonal_stroke().size() == 3);
    HR_CHECK(!diagonal_stroke().empty());

    const Stroke timed({Point{0.0, 0.0, 0.2, 0}, Point{0.1, 0.0, 0.8, 40}});
    HR_CHECK(timed.duration_ms() == 40);
    HR_CHECK_NEAR(timed.mean_pressure(), 0.5);

    const auto box = diagonal_stroke().bounds();
    HR_CHECK(box.has_value());
    HR_CHECK_NEAR(box->min_x, 0.0);
    HR_CHECK_NEAR(box->min_y, 0.0);
    HR_CHECK_NEAR(box->max_x, 0.3);
    HR_CHECK_NEAR(box->max_y, 0.8);
    HR_CHECK_NEAR(box->width(), 0.3);
    HR_CHECK_NEAR(box->height(), 0.8);
    HR_CHECK_NEAR(box->longest_side(), 0.8);

    // ---- 空笔画必须被拒绝 ----
    bool threw = false;
    try {
        const Stroke empty{std::vector<Point>{}};
        (void)empty;
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    HR_CHECK(threw);

    // ---- 仿射变换 ----
    const Stroke moved = Stroke({Point{0.0, 0.0}}).translated(0.25, -0.5);
    HR_CHECK_NEAR(moved.points().front().x, 0.25);
    HR_CHECK_NEAR(moved.points().front().y, -0.5);

    const Stroke skewed = Stroke({Point{0.0, 1.0}}).sheared(0.2, 0.0);
    HR_CHECK_NEAR(skewed.points().front().x, 0.2);
    HR_CHECK_NEAR(skewed.points().front().y, 1.0);

    // ---- StrokeSet 聚合量 ----
    const StrokeSet glyph({diagonal_stroke(), Stroke({Point{0.5, 0.5}})}, "永");
    HR_CHECK(glyph.stroke_count() == 2);
    HR_CHECK(glyph.point_count() == 4);
    HR_CHECK_NEAR(glyph.total_length(), 0.9);
    HR_CHECK(glyph.character() == "永");

    HR_CHECK(!StrokeSet{}.bounds().has_value());

    const StrokeSet spread({Stroke({Point{0.2, 0.2}}), Stroke({Point{0.9, 0.7}})});
    const auto spread_box = spread.bounds();
    HR_CHECK(spread_box.has_value());
    HR_CHECK_NEAR(spread_box->min_x, 0.2);
    HR_CHECK_NEAR(spread_box->min_y, 0.2);
    HR_CHECK_NEAR(spread_box->max_x, 0.9);
    HR_CHECK_NEAR(spread_box->max_y, 0.7);

    // ---- fit_to_box ----
    const StrokeSet sample({Stroke({Point{0.2, 0.3}, Point{0.8, 0.9}})});

    const auto flush = sample.fit_to_box(1.0, 0.0).bounds();
    HR_CHECK(flush.has_value());
    HR_CHECK_NEAR(flush->min_x, 0.0);
    HR_CHECK_NEAR(flush->min_y, 0.0);
    HR_CHECK_NEAR(flush->max_x, 1.0);
    HR_CHECK_NEAR(flush->max_y, 1.0);

    const auto padded = sample.fit_to_box(1.0, 0.1).bounds();
    HR_CHECK(padded.has_value());
    HR_CHECK_NEAR(padded->min_x, 0.1);
    HR_CHECK_NEAR(padded->min_y, 0.1);
    HR_CHECK_NEAR(padded->max_x, 0.9);
    HR_CHECK_NEAR(padded->max_y, 0.9);

    // 宽高比 1:2 的字形缩放后必须保持，不能拉伸
    const StrokeSet tall({Stroke({Point{0.0, 0.0}, Point{0.4, 0.8}})});
    const auto tall_box = tall.fit_to_box(1.0, 0.0).bounds();
    HR_CHECK(tall_box.has_value());
    HR_CHECK_NEAR(tall_box->width() / tall_box->height(), 0.5);
    HR_CHECK_NEAR(tall_box->height(), 1.0);

    // 退化为单点时必须返回空集而不是崩溃
    HR_CHECK(StrokeSet({Stroke({Point{0.5, 0.5}})}).fit_to_box().stroke_count() == 0);
    HR_CHECK(StrokeSet{}.fit_to_box().stroke_count() == 0);

    // 缩放不得改变笔画数量与顺序
    const auto fitted = glyph.fit_to_box(1.0);
    HR_CHECK(fitted.stroke_count() == glyph.stroke_count());
    HR_CHECK(fitted.character() == "永");

    return hrtest::summary("trajectory");
}
