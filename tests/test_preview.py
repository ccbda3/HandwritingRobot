"""SVG 预览测试。

重点验证**预览与 G 代码的版面一致** —— 预览若与实际输出不符，
它就失去了全部意义，而且这种偏差很难被肉眼发现。
"""

from __future__ import annotations

import re
import xml.etree.ElementTree as ET

import pytest

from handwriting.gcode import PlotSettings, char_origin, page_size, strokes_to_gcode
from handwriting.preview import SvgStyle, to_svg
from handwriting.stroke import Point, Stroke, StrokeSet


def glyph(char: str = "永", points: int = 5) -> StrokeSet:
    return StrokeSet(
        [Stroke([Point(i / (points - 1), 0.5, 0.5, i * 10) for i in range(points)])], char
    )


def parse(svg: str) -> ET.Element:
    return ET.fromstring(svg)


class TestDocumentStructure:
    def test_produces_well_formed_xml(self) -> None:
        root = parse(to_svg([[glyph()]]))
        assert root.tag.endswith("svg")

    def test_viewbox_matches_page_size(self) -> None:
        settings = PlotSettings(chars_per_line=10)
        svg = to_svg([[glyph()]], settings)
        width, height = page_size(1, settings)
        match = re.search(r'viewBox="0 0 ([\d.]+) ([\d.]+)"', svg)
        assert match is not None
        assert float(match.group(1)) == pytest.approx(width, abs=0.01)
        assert float(match.group(2)) == pytest.approx(height, abs=0.01)

    def test_has_paper_background(self) -> None:
        root = parse(to_svg([[glyph()]]))
        rects = [el for el in root if el.tag.endswith("rect")]
        assert any(el.get("fill") == SvgStyle().paper for el in rects)

    def test_empty_input_still_produces_valid_svg(self) -> None:
        root = parse(to_svg([]))
        assert root.tag.endswith("svg")


class TestLayoutConsistency:
    """预览与 G 代码必须使用同一套版面计算。"""

    def test_preview_matches_gcode_origins(self) -> None:
        settings = PlotSettings(char_size_mm=20.0, char_gap_mm=3.0, margin_x_mm=10.0)
        rows = [[glyph("永"), glyph("西")]]
        svg = to_svg(rows, settings, SvgStyle(show_grid=True, pressure_width=0.0))

        root = parse(svg)
        rects = [el for el in root if el.tag.endswith("rect")]
        grid = [el for el in rects if el.get("fill") == "none"]
        assert len(grid) == 2

        for index, element in enumerate(grid):
            expected_x, expected_y = char_origin(0, index, settings)
            assert float(element.get("x")) == pytest.approx(expected_x, abs=0.01)
            assert float(element.get("y")) == pytest.approx(expected_y, abs=0.01)
            assert float(element.get("width")) == pytest.approx(settings.char_size_mm, abs=0.01)

    def test_gcode_and_preview_agree_on_page_extent(self) -> None:
        settings = PlotSettings(char_size_mm=20.0, margin_x_mm=10.0, chars_per_line=10)
        rows = [[glyph() for _ in range(3)]]

        program = strokes_to_gcode(rows, settings)
        max_x = 0.0
        for line in program.splitlines():
            if line.startswith("G1 X"):
                max_x = max(max_x, float(line.split()[1][1:]))

        width, _ = page_size(1, settings)
        assert max_x <= width
        # 最后一个字的右边界应当接近但不超过幅面
        last_x, _ = char_origin(0, 2, settings)
        assert max_x == pytest.approx(last_x + settings.char_size_mm * 0.9, rel=0.15)


class TestRendering:
    def test_one_polyline_per_stroke_when_pressure_is_flat(self) -> None:
        svg = to_svg([[glyph()]], style=SvgStyle(pressure_width=0.0))
        assert svg.count("<polyline") == 1

    def test_pressure_variation_splits_into_segments(self) -> None:
        stroke = Stroke([Point(i / 20, 0.5, 0.1 if i < 10 else 0.9, i * 10) for i in range(21)])
        svg = to_svg([[StrokeSet([stroke])]])
        assert svg.count("<polyline") >= 2

    def test_stroke_width_reflects_pressure(self) -> None:
        low = to_svg([[StrokeSet([Stroke([Point(0.0, 0.5, 0.1, 0), Point(1.0, 0.5, 0.1, 10)])])]])
        high = to_svg([[StrokeSet([Stroke([Point(0.0, 0.5, 0.9, 0), Point(1.0, 0.5, 0.9, 10)])])]])

        def width_of(svg: str) -> float:
            # 必须定位到 polyline：文档里第一个 stroke-width 属于字格边框
            match = re.search(r'<polyline[^>]*stroke-width="([\d.]+)"', svg)
            assert match is not None
            return float(match.group(1))

        assert width_of(high) > width_of(low) * 1.5

    def test_single_point_stroke_becomes_circle(self) -> None:
        svg = to_svg([[StrokeSet([Stroke([Point(0.5, 0.5, 0.5, 0)])])]])
        assert "<circle" in svg

    def test_stroke_order_labels_are_optional(self) -> None:
        rows = [[StrokeSet([Stroke([Point(0.0, 0.5, 0.5, 0)]), Stroke([Point(1.0, 0.5, 0.5, 0)])])]]
        without = to_svg(rows, style=SvgStyle(show_stroke_order=False))
        with_labels = to_svg(rows, style=SvgStyle(show_stroke_order=True))
        assert "<text" not in without
        assert with_labels.count("<text") == 2

    def test_grid_can_be_disabled(self) -> None:
        with_grid = to_svg([[glyph()]], style=SvgStyle(show_grid=True))
        without = to_svg([[glyph()]], style=SvgStyle(show_grid=False))
        assert with_grid.count('fill="none"') > without.count('fill="none"')

    def test_numbers_are_trimmed(self) -> None:
        """去掉无意义的尾随零，避免 SVG 体积无谓膨胀。"""
        svg = to_svg([[glyph()]])
        assert "0.000000" not in svg
        assert not re.search(r'"\d+\.0+"', svg)


class TestSvgIsUsable:
    def test_output_can_be_written_and_reparsed(self, tmp_path) -> None:
        from pathlib import Path

        path = Path(tmp_path) / "preview.svg"
        path.write_text(to_svg([[glyph(), glyph("西")]]), encoding="utf-8")
        assert parse(path.read_text(encoding="utf-8")).tag.endswith("svg")

    def test_all_coordinates_are_finite(self) -> None:
        svg = to_svg([[glyph()]])
        for match in re.finditer(r'"([-\d.,\s]+)"', svg):
            for token in re.split(r"[,\s]+", match.group(1).strip()):
                if token:
                    assert float(token) == float(token)  # 非 NaN
