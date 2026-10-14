"""文本渲染测试：倾斜、抖动、笔压、时序与排版。"""

from __future__ import annotations

import random

import pytest

from handwriting.glyph import Glyph, GlyphNotFoundError
from handwriting.render import (
    RenderSettings,
    apply_jitter,
    apply_pressure,
    apply_slant,
    apply_timing,
    render_glyph,
    render_text,
)
from handwriting.stroke import Point, Stroke, StrokeSet
from handwriting.style import extract


class FakeSource:
    """内存字形源，避免测试依赖网络。"""

    def __init__(self, chars: str) -> None:
        self.glyphs = {
            char: Glyph(char, (((100.0, 800.0), (500.0, 400.0), (900.0, 100.0)),)) for char in chars
        }

    def load(self, char: str) -> Glyph:
        if char not in self.glyphs:
            raise GlyphNotFoundError(f"字形库里没有 {char!r}")
        return self.glyphs[char]


def vertical_stroke() -> Stroke:
    return Stroke([Point(0.5, 0.0, 0.5, 0), Point(0.5, 1.0, 0.5, 100)])


class TestSlant:
    def test_zero_slant_is_identity(self) -> None:
        original = StrokeSet([vertical_stroke()])
        assert apply_slant(original, 0.0) is original

    def test_right_lean_moves_top_right(self) -> None:
        """右倾 = 顶偏右、底偏左。"""
        slanted = apply_slant(StrokeSet([vertical_stroke()]), 10.0)
        points = slanted.strokes[0].points
        assert points[0].x > 0.5  # 顶点（y=0）右移
        assert points[-1].x < 0.5  # 底点（y=1）左移

    def test_left_lean_moves_top_left(self) -> None:
        slanted = apply_slant(StrokeSet([vertical_stroke()]), -10.0)
        points = slanted.strokes[0].points
        assert points[0].x < 0.5
        assert points[-1].x > 0.5

    def test_shear_is_around_centre(self) -> None:
        """绕中心错切：位于中线的点不应移动。"""
        stroke = Stroke([Point(0.5, 0.5, 0.5, 0)])
        slanted = apply_slant(StrokeSet([stroke]), 15.0)
        assert slanted.strokes[0].points[0].x == pytest.approx(0.5)

    def test_angle_matches_requested_value(self) -> None:
        slanted = apply_slant(StrokeSet([vertical_stroke()]), 8.0)
        assert extract(slanted).slant_deg == pytest.approx(8.0, abs=0.2)

    def test_sign_convention_matches_style_module(self) -> None:
        """render 的倾斜与 style 的倾角必须用同一套符号约定。

        两边搞反的话，界面显示「右倾 10°」而实际输出是左倾 ——
        这种错误肉眼很难发现，因为两种倾斜看起来都「像斜体」。
        """
        for requested in (-12.0, -5.0, 5.0, 12.0):
            slanted = apply_slant(StrokeSet([vertical_stroke()]), requested)
            assert extract(slanted).slant_deg == pytest.approx(requested, abs=0.3)


class TestJitter:
    def test_zero_amplitude_is_identity(self) -> None:
        original = StrokeSet([vertical_stroke()])
        assert apply_jitter(original, 0.0, random.Random(0)) is original

    def test_perturbs_points(self) -> None:
        original = StrokeSet([vertical_stroke()])
        jittered = apply_jitter(original, 0.01, random.Random(0))
        assert jittered.strokes[0].points[0].x != original.strokes[0].points[0].x

    def test_same_seed_gives_same_result(self) -> None:
        original = StrokeSet([vertical_stroke()])
        first = apply_jitter(original, 0.01, random.Random(42))
        second = apply_jitter(original, 0.01, random.Random(42))
        assert first.strokes[0].points == second.strokes[0].points

    def test_amplitude_is_respected(self) -> None:
        """位移应大体落在 3 倍标准差内。"""
        original = StrokeSet([vertical_stroke()])
        amplitude = 0.005
        jittered = apply_jitter(original, amplitude, random.Random(7))
        for before, after in zip(
            original.strokes[0].points, jittered.strokes[0].points, strict=False
        ):
            assert abs(after.x - before.x) < amplitude * 5

    def test_pressure_is_untouched(self) -> None:
        original = StrokeSet([vertical_stroke()])
        jittered = apply_jitter(original, 0.01, random.Random(1))
        assert jittered.strokes[0].points[0].pressure == pytest.approx(0.5)


class TestPressure:
    def test_long_stroke_is_heavier_at_both_ends(self) -> None:
        """楷书横竖的提按：起笔顿、行笔提、收笔顿。"""
        stroke = Stroke([Point(i / 10, 0.5, 0.5, i * 10) for i in range(11)])
        result = apply_pressure(StrokeSet([stroke]), RenderSettings())
        pressures = [p.pressure for p in result.strokes[0].points]
        assert pressures[0] > pressures[len(pressures) // 2]
        assert pressures[-1] > pressures[len(pressures) // 2]
        assert pressures[0] == pytest.approx(pressures[-1], abs=1e-9)

    def test_dot_is_heavier_in_the_middle(self) -> None:
        """点：单峰，中段最重。"""
        dot = Stroke([Point(0.5 + i * 0.005, 0.5, 0.5, i * 10) for i in range(6)])
        result = apply_pressure(StrokeSet([dot]), RenderSettings())
        pressures = [p.pressure for p in result.strokes[0].points]
        assert pressures[len(pressures) // 2] > pressures[0]

    def test_single_point_stroke_gets_peak_pressure(self) -> None:
        result = apply_pressure(StrokeSet([Stroke([Point(0.5, 0.5, 0.5, 0)])]), RenderSettings())
        expected = RenderSettings().pressure_base + RenderSettings().pressure_amplitude
        assert result.strokes[0].points[0].pressure == pytest.approx(expected)

    def test_pressure_stays_within_range(self) -> None:
        settings = RenderSettings(pressure_base=0.9, pressure_amplitude=0.5)
        stroke = Stroke([Point(i / 10, 0.5, 0.5, i * 10) for i in range(11)])
        result = apply_pressure(StrokeSet([stroke]), settings)
        for point in result.strokes[0].points:
            assert 0.0 <= point.pressure <= 1.0

    def test_zero_amplitude_gives_constant_pressure(self) -> None:
        settings = RenderSettings(pressure_amplitude=0.0, pressure_base=0.6)
        stroke = Stroke([Point(i / 10, 0.5, 0.5, i * 10) for i in range(11)])
        result = apply_pressure(StrokeSet([stroke]), settings)
        for point in result.strokes[0].points:
            assert point.pressure == pytest.approx(0.6)


class TestTiming:
    def test_time_follows_arc_length(self) -> None:
        # 总长 0.5，速度 2.5 单位/秒 → 200 ms
        stroke = Stroke(
            [Point(0.0, 0.0, 0.5, 0), Point(0.25, 0.0, 0.5, 0), Point(0.5, 0.0, 0.5, 0)]
        )
        result = apply_timing(StrokeSet([stroke]), RenderSettings(writing_speed=2.5))
        times = [p.t for p in result.strokes[0].points]
        assert times[0] == 0
        assert times[1] == pytest.approx(100, abs=1)
        assert times[2] == pytest.approx(200, abs=1)

    def test_time_is_monotonic(self) -> None:
        stroke = Stroke([Point(i / 10, 0.0, 0.5, 0) for i in range(11)])
        result = apply_timing(StrokeSet([stroke]), RenderSettings())
        times = [p.t for p in result.strokes[0].points]
        assert times == sorted(times)

    def test_faster_speed_shortens_time(self) -> None:
        stroke = Stroke([Point(0.0, 0.0, 0.5, 0), Point(1.0, 0.0, 0.5, 0)])
        slow = apply_timing(StrokeSet([stroke]), RenderSettings(writing_speed=1.0))
        fast = apply_timing(StrokeSet([stroke]), RenderSettings(writing_speed=4.0))
        assert fast.strokes[0].points[-1].t < slow.strokes[0].points[-1].t

    def test_each_stroke_starts_at_zero(self) -> None:
        """笔内时间从 0 起算，笔间间隔由采集格式的 strokeStartT 表达。"""
        strokes = StrokeSet(
            [
                Stroke([Point(0.0, 0.0, 0.5, 0), Point(0.5, 0.0, 0.5, 0)]),
                Stroke([Point(0.0, 0.5, 0.5, 0), Point(0.5, 0.5, 0.5, 0)]),
            ]
        )
        result = apply_timing(strokes, RenderSettings())
        for stroke in result.strokes:
            assert stroke.points[0].t == 0


class TestRenderGlyph:
    def test_produces_expected_stroke_count(self) -> None:
        glyph = Glyph("永", (((100.0, 800.0), (900.0, 100.0)),))
        result = render_glyph(glyph, RenderSettings(), random.Random(0))
        assert result.stroke_count == 1
        assert result.char == "永"

    def test_features_are_extractable(self) -> None:
        """渲染结果必须能直接喂给风格特征提取 —— 这是后续做质量对比的前提。"""
        glyph = Glyph("永", (((100.0, 800.0), (500.0, 400.0), (900.0, 100.0)),))
        result = render_glyph(glyph, RenderSettings(), random.Random(0))
        features = extract(result)
        assert features.stroke_count == 1
        assert features.speed_mean is not None
        assert features.pressure_mean is not None


class TestRenderText:
    def test_renders_each_character(self) -> None:
        result = render_text("永西叶", FakeSource("永西叶"))
        assert result.char_count == 3
        assert result.missing == []
        assert len(result.rows) == 1

    def test_newline_forces_a_break(self) -> None:
        result = render_text("永西\n叶好", FakeSource("永西叶好"))
        assert len(result.rows) == 2
        assert [len(row) for row in result.rows] == [2, 2]

    def test_chars_per_line_wraps(self) -> None:
        result = render_text("永西叶好冬花", FakeSource("永西叶好冬花"), chars_per_line=2)
        assert [len(row) for row in result.rows] == [2, 2, 2]

    def test_whitespace_is_skipped(self) -> None:
        result = render_text("永 西\t叶", FakeSource("永西叶"))
        assert result.char_count == 3

    def test_missing_characters_are_skipped_not_fatal(self) -> None:
        """生僻字或标点缺失不该让整段文本渲染不出来。"""
        result = render_text("永龘西", FakeSource("永西"))
        assert result.char_count == 2
        assert result.missing == ["龘"]

    def test_missing_list_is_deduplicated(self) -> None:
        result = render_text("龘龘龘永", FakeSource("永"))
        assert result.missing == ["龘"]

    def test_empty_text_gives_empty_result(self) -> None:
        result = render_text("", FakeSource(""))
        assert result.is_empty
        assert result.rows == []

    def test_only_newlines_gives_empty_result(self) -> None:
        result = render_text("\n\n\n", FakeSource("永"))
        assert result.is_empty

    def test_rejects_invalid_chars_per_line(self) -> None:
        with pytest.raises(ValueError, match="chars_per_line"):
            render_text("永", FakeSource("永"), chars_per_line=0)

    def test_jitter_seed_is_reproducible(self) -> None:
        settings = RenderSettings(jitter=0.005, jitter_seed=99)
        first = render_text("永西", FakeSource("永西"), settings)
        second = render_text("永西", FakeSource("永西"), settings)
        assert first.rows[0][0].strokes[0].points == second.rows[0][0].strokes[0].points

    def test_render_result_counts(self) -> None:
        result = render_text("永西", FakeSource("永西"))
        assert result.stroke_count == 2
        assert result.point_count > 0


class TestSlantAndStyleAgree:
    def test_rendered_glyph_slant_matches_requested(self) -> None:
        """端到端确认：请求 6° 右倾，渲染后量出来就该是 6° 左右。"""
        glyph = Glyph("测", (((500.0, 850.0), (500.0, 100.0)),))
        result = render_glyph(glyph, RenderSettings(slant_deg=6.0), random.Random(0))
        assert extract(result).slant_deg == pytest.approx(6.0, abs=0.3)

    def test_tilted_glyph_is_detectably_different(self) -> None:
        glyph = Glyph("测", (((500.0, 850.0), (500.0, 100.0)),))
        upright = render_glyph(glyph, RenderSettings(slant_deg=0.0), random.Random(0))
        leaning = render_glyph(glyph, RenderSettings(slant_deg=15.0), random.Random(0))
        assert abs(extract(leaning).slant_deg - extract(upright).slant_deg) > 10.0

    def test_shear_preserves_stroke_length_approximately(self) -> None:
        """错切是保面积的线性变换，笔画长度不应发生数量级变化。"""
        glyph = Glyph("测", (((500.0, 850.0), (500.0, 100.0)),))
        upright = render_glyph(glyph, RenderSettings(slant_deg=0.0), random.Random(0))
        leaning = render_glyph(glyph, RenderSettings(slant_deg=15.0), random.Random(0))
        ratio = leaning.total_length / upright.total_length
        assert 0.9 < ratio < 1.4
