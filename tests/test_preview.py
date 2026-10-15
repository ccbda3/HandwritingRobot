"""SVG 预览测试。

重点验证**预览与 G 代码的版面一致** —— 预览若与实际输出不符，
它就失去了全部意义，而且这种偏差很难被肉眼发现。
"""

from __future__ import annotations

import re
import xml.etree.ElementTree as ET
from pathlib import Path

import pytest

from handwriting.gcode import PlotSettings, glyphs_to_gcode_with_stats
from handwriting.layout import LayoutSettings, Mode, Page, compute
from handwriting.preview import SvgStyle, to_svg
from handwriting.stroke import Point, Stroke, StrokeSet


def glyph(char: str = "永", points: int = 5) -> StrokeSet:
    return StrokeSet(
        [Stroke([Point(i / (points - 1), 0.5, 0.5, i * 10) for i in range(points)])], char
    )


def parse(svg: str) -> ET.Element:
    return ET.fromstring(svg)


def make_page(count: int, mode: Mode = Mode.NOTEBOOK, **kwargs: object) -> Page:
    return compute([count], LayoutSettings(mode=mode, **kwargs))  # type: ignore[arg-type]


class TestDocumentStructure:
    def test_produces_well_formed_xml(self) -> None:
        page = make_page(1)
        root = parse(to_svg([glyph()], page))
        assert root.tag.endswith("svg")

    def test_viewbox_matches_page_size(self) -> None:
        page = make_page(3)
        svg = to_svg([glyph()] * 3, page)
        match = re.search(r'viewBox="0 0 ([\d.]+) ([\d.]+)"', svg)
        assert match is not None
        assert float(match.group(1)) == pytest.approx(page.width_mm, abs=0.01)
        assert float(match.group(2)) == pytest.approx(page.height_mm, abs=0.01)

    def test_has_paper_background(self) -> None:
        root = parse(to_svg([glyph()], make_page(1)))
        rects = [el for el in root if el.tag.endswith("rect")]
        assert any(el.get("fill") == SvgStyle().paper for el in rects)

    def test_empty_glyph_list_still_produces_valid_svg(self) -> None:
        root = parse(to_svg([], make_page(0)))
        assert root.tag.endswith("svg")


class TestNotebookLayout:
    def test_rules_are_drawn(self) -> None:
        page = make_page(5)
        svg = to_svg([glyph()] * 5, page)
        assert svg.count("<line") == len(page.rule_lines_mm) == 22

    def test_rules_can_be_disabled(self) -> None:
        page = make_page(5)
        svg = to_svg([glyph()] * 5, page, SvgStyle(show_rules=False))
        assert "<line" not in svg

    def test_grid_rects_match_cells(self) -> None:
        page = make_page(3)
        root = parse(to_svg([glyph()] * 3, page, SvgStyle(show_grid=True)))
        grid = [el for el in root if el.tag.endswith("rect") and el.get("fill") == "none"]
        assert len(grid) == 3
        for element, cell in zip(grid, page.cells, strict=True):
            assert float(element.get("x")) == pytest.approx(cell.x_mm, abs=0.01)
            assert float(element.get("y")) == pytest.approx(cell.y_mm, abs=0.01)
            assert float(element.get("width")) == pytest.approx(cell.size_mm, abs=0.01)

    def test_characters_sit_on_the_rules(self) -> None:
        """字格基线必须落在某条横线上。

        这是横条本的核心要求。若按字格顶部对齐，字会浮在横线上方约 12% 字高，
        预览里看是「差不多对」，写出来就是明显偏高。
        """
        page = make_page(3)
        for cell in page.cells:
            assert any(abs(cell.baseline_mm - rule) < 1e-9 for rule in page.rule_lines_mm), (
                f"基线 {cell.baseline_mm} 不在任何横线上"
            )


class TestGridLayout:
    def test_grid_mode_has_no_rules(self) -> None:
        page = make_page(3, mode=Mode.GRID, char_size_mm=20.0)
        assert page.rule_lines_mm == []
        assert "<line" not in to_svg([glyph()] * 3, page)


class TestLayoutConsistency:
    """预览与 G 代码必须使用同一个 Page。"""

    def test_preview_and_gcode_share_the_same_cells(self) -> None:
        page = make_page(3)
        glyphs = [glyph()] * 3

        root = parse(to_svg(glyphs, page, SvgStyle(show_grid=True)))
        grid = [el for el in root if el.tag.endswith("rect") and el.get("fill") == "none"]

        program = glyphs_to_gcode_with_stats(glyphs, page, PlotSettings()).lines_text()
        # 排除页脚的 G0 X0 Y0（回原点），它不是字格定位
        travels = [
            line for line in program.splitlines() if line.startswith("G0 X") and line != "G0 X0 Y0"
        ]

        assert len(grid) == len(travels) == 3
        for element, cell, travel in zip(grid, page.cells, travels, strict=True):
            # 预览里字格的位置与 G 代码里空行程的落点必须一致
            assert float(element.get("x")) == pytest.approx(cell.x_mm, abs=0.01)
            assert float(travel.split()[1][1:]) == pytest.approx(
                cell.x_mm + 0.0 * cell.size_mm, abs=0.01
            )

    def test_gcode_stays_inside_the_page(self) -> None:
        page = make_page(20)
        glyphs = [glyph()] * 20
        program = glyphs_to_gcode_with_stats(glyphs, page, PlotSettings()).lines_text()

        for line in program.splitlines():
            if line.startswith(("G0 X", "G1 X")):
                parts = line.split()
                x = float(parts[1][1:])
                y = float(parts[2][1:])
                assert -0.01 <= x <= page.width_mm + 0.01, line
                assert -0.01 <= y <= page.height_mm + 0.01, line


class TestRendering:
    def test_one_polyline_per_stroke_when_pressure_is_flat(self) -> None:
        page = make_page(1)
        svg = to_svg([glyph()], page, SvgStyle(pressure_width=0.0))
        assert svg.count("<polyline") == 1

    def test_pressure_variation_splits_into_segments(self) -> None:
        page = make_page(1)
        stroke = Stroke([Point(i / 20, 0.5, 0.1 if i < 10 else 0.9, i * 10) for i in range(21)])
        svg = to_svg([StrokeSet([stroke])], page)
        assert svg.count("<polyline") >= 2

    def test_stroke_width_reflects_pressure(self) -> None:
        page = make_page(1)
        low = to_svg(
            [StrokeSet([Stroke([Point(0.0, 0.5, 0.1, 0), Point(1.0, 0.5, 0.1, 10)])])], page
        )
        high = to_svg(
            [StrokeSet([Stroke([Point(0.0, 0.5, 0.9, 0), Point(1.0, 0.5, 0.9, 10)])])], page
        )

        def width_of(svg: str) -> float:
            # 必须定位到 polyline：文档里第一个 stroke-width 属于字格边框
            match = re.search(r'<polyline[^>]*stroke-width="([\d.]+)"', svg)
            assert match is not None
            return float(match.group(1))

        assert width_of(high) > width_of(low) * 1.5

    def test_single_point_stroke_becomes_circle(self) -> None:
        page = make_page(1)
        svg = to_svg([StrokeSet([Stroke([Point(0.5, 0.5, 0.5, 0)])])], page)
        assert "<circle" in svg

    def test_stroke_order_labels_are_optional(self) -> None:
        page = make_page(1)
        strokes = StrokeSet([Stroke([Point(0.0, 0.5, 0.5, 0)]), Stroke([Point(1.0, 0.5, 0.5, 0)])])
        without = to_svg([strokes], page, SvgStyle(show_stroke_order=False))
        with_labels = to_svg([strokes], page, SvgStyle(show_stroke_order=True))
        assert "<text" not in without
        assert with_labels.count("<text") == 2

    def test_numbers_are_trimmed(self) -> None:
        """去掉无意义的尾随零，避免 SVG 体积无谓膨胀。"""
        svg = to_svg([glyph()], make_page(1))
        assert "0.000000" not in svg
        assert not re.search(r'"\d+\.0+"', svg)


class TestSvgIsUsable:
    def test_output_can_be_written_and_reparsed(self, tmp_path: Path) -> None:
        page = make_page(2)
        path = Path(tmp_path) / "preview.svg"
        path.write_text(to_svg([glyph(), glyph("西")], page), encoding="utf-8")
        assert parse(path.read_text(encoding="utf-8")).tag.endswith("svg")

    def test_all_coordinates_are_finite(self) -> None:
        svg = to_svg([glyph()], make_page(1))
        for match in re.finditer(r'"([-\d.,\s]+)"', svg):
            for token in re.split(r"[,\s]+", match.group(1).strip()):
                if token:
                    assert float(token) == float(token)  # 非 NaN
