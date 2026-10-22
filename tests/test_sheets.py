"""ShapeSheet 编排测试：把任意图形摆上版面并出 G 代码 / 预览。"""

from __future__ import annotations

from handwriting.gcode import PlotSettings
from handwriting.layout import LayoutSettings, Mode
from handwriting.shapes import compose_table
from handwriting.sheets import ShapeSheet, compose


def _settings() -> LayoutSettings:
    return LayoutSettings(mode=Mode.GRID)


def test_sheet_single_shape_layout() -> None:
    sheet = ShapeSheet([compose_table(2, 2)])
    fitted, page = sheet.layout(_settings())
    assert len(fitted) == 1
    assert len(page.cells) == 1


def test_sheet_near_square_grid() -> None:
    sheet = ShapeSheet([compose_table(2, 2) for _ in range(4)])
    _, page = sheet.layout(_settings())
    assert len(page.cells) == 4
    # 2×2 网格，宽应明显小于「一行 4 个」的宽
    assert page.width_mm < 200


def test_sheet_to_gcode_and_svg() -> None:
    sheet = ShapeSheet([compose_table(3, 4)])
    gcode = sheet.to_gcode(PlotSettings(), _settings())
    svg = sheet.to_svg(None, _settings())
    assert "G1" in gcode
    assert "G0 Z" in gcode
    assert "<svg" in svg
    assert svg.count("polyline") > 0


def test_compose_convenience() -> None:
    gcode, svg = compose([compose_table(2, 2)], PlotSettings(), _settings())
    assert "G1" in gcode
    assert "<svg" in svg


def test_sheet_empty() -> None:
    sheet = ShapeSheet([])
    assert sheet.count == 0
    gcode = sheet.to_gcode(PlotSettings(), _settings())
    assert "G21" in gcode  # 仅有表头
