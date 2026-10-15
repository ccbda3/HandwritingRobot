"""G 代码生成测试。"""

from __future__ import annotations

import math

import pytest

from handwriting.gcode import PlotSettings, strokes_to_gcode, strokes_to_gcode_with_stats
from handwriting.stroke import Point, Stroke, StrokeSet


def single_char() -> StrokeSet:
    return StrokeSet([Stroke([Point(0.1, 0.2), Point(0.3, 0.4)])], "永")


class TestProgramStructure:
    def test_header_and_footer_present(self) -> None:
        text = strokes_to_gcode([[single_char()]])
        lines = text.strip().splitlines()
        assert lines[:3] == ["G21", "G90", "G17"]
        assert lines[-2:] == ["M5", "G0 X0 Y0"]
        assert text.endswith("\n")

    def test_pen_down_before_drawing_and_up_after(self) -> None:
        lines = strokes_to_gcode([[single_char()]]).strip().splitlines()
        assert lines.count("G0 Z0") == 1
        assert lines.count("G0 Z3") == 1
        assert lines.index("G0 Z0") < lines.index("G1 X16 Y18 F1200")
        assert lines.index("G0 Z3") > lines.index("G1 X16 Y18 F1200")

    def test_travel_precedes_first_draw_point(self) -> None:
        lines = strokes_to_gcode([[single_char()]]).strip().splitlines()
        assert lines[3] == "G0 X12 Y14"


class TestLayout:
    def test_second_char_is_offset_by_size_plus_gap(self) -> None:
        row = [single_char(), single_char()]
        lines = strokes_to_gcode([row]).strip().splitlines()
        travels = [line for line in lines if line.startswith("G0 X") and line != "G0 X0 Y0"]
        assert travels[0] == "G0 X12 Y14"
        # 20mm 字宽 + 3mm 间距 => 第二个字原点 x = 10 + 23 = 33，首点 x = 35
        assert travels[1] == "G0 X35 Y14"

    def test_second_row_is_offset_by_line_gap(self) -> None:
        lines = strokes_to_gcode([[single_char()], [single_char()]]).strip().splitlines()
        travels = [line for line in lines if line.startswith("G0 X") and line != "G0 X0 Y0"]
        assert travels[1] == "G0 X12 Y44"


class TestGeometryAndStats:
    def test_lengths_and_estimate(self) -> None:
        result = strokes_to_gcode_with_stats([[single_char()]])
        stats = result.stats
        assert stats.stroke_count == 1
        assert stats.point_count == 2
        assert stats.draw_length_mm == pytest.approx(math.sqrt(32))
        assert stats.travel_length_mm == pytest.approx(math.sqrt(20))
        expected_seconds = math.sqrt(32) / 20.0 + math.sqrt(20) / 50.0
        assert stats.estimated_seconds == pytest.approx(expected_seconds)

    def test_two_stroke_char_toggles_pen_twice(self) -> None:
        char = StrokeSet([Stroke([Point(0.1, 0.1)]), Stroke([Point(0.9, 0.9)])])
        result = strokes_to_gcode_with_stats([[char]])
        lines = result.stats.lines
        assert lines.count("G0 Z0") == 2
        assert lines.count("G0 Z3") == 2


class TestSettings:
    def test_spindle_mode_uses_m3_m5(self) -> None:
        settings = PlotSettings(use_z_axis=False)
        lines = strokes_to_gcode([[single_char()]], settings).strip().splitlines()
        assert "M3 S1000" in lines
        assert "G0 Z0" not in lines

    def test_spindle_mode_does_not_emit_duplicate_m5(self) -> None:
        """抬笔已经是 M5，页脚再写一次会输出两个连续 M5。"""
        settings = PlotSettings(use_z_axis=False)
        lines = strokes_to_gcode([[single_char()]], settings).strip().splitlines()
        for before, after in zip(lines, lines[1:], strict=False):
            assert not (before == "M5" and after == "M5")

    def test_z_mode_keeps_footer_m5(self) -> None:
        lines = strokes_to_gcode([[single_char()]]).strip().splitlines()
        assert lines[-2] == "M5"

    def test_custom_feedrate_is_emitted(self) -> None:
        settings = PlotSettings(feed_draw=600.0)
        text = strokes_to_gcode([[single_char()]], settings)
        assert "F600" in text

    def test_decimal_places_are_trimmed(self) -> None:
        settings = PlotSettings(decimals=3)
        text = strokes_to_gcode([[single_char()]], settings)
        assert "X12 " in text  # 12.000 应输出为 12，而不是 12.000

    def test_empty_input_produces_valid_program(self) -> None:
        text = strokes_to_gcode([])
        assert text.strip().splitlines() == ["G21", "G90", "G17", "M5", "G0 X0 Y0"]
