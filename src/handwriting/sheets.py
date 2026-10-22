"""任意图形的版面编排 —— 把若干 ``StrokeSet`` 摆上纸面并出 G 代码 / 预览。

## 与文字管线的对称关系

文字链路是 ``render_text``（出字形 ``StrokeSet``）→ :mod:`handwriting.layout`
（算字格 ``Page``）→ :func:`handwriting.gcode.glyphs_to_gcode_with_stats` /
:func:`handwriting.preview.to_svg`（出机器指令 / 预览）。

本模块对**非文字图形**做完全同样的事：每个图形是一个 ``StrokeSet``，按网格
摆进 ``Page`` 的 ``Cell``，然后复用上面两个已有的出口函数。这样预览与机器
实际走位必然一致，且不用为图形单独写一套 G 代码生成。
"""

from __future__ import annotations

import math
from collections.abc import Sequence

from handwriting.gcode import PlotSettings, glyphs_to_gcode_with_stats
from handwriting.layout import LayoutSettings, compute
from handwriting.preview import SvgStyle, to_svg
from handwriting.stroke import StrokeSet


class ShapeSheet:
    """一页任意图形：每个元素是一个 ``StrokeSet``（已归一化到 0~1）。"""

    def __init__(self, shapes: Sequence[StrokeSet], labels: Sequence[str] | None = None) -> None:
        self.shapes: list[StrokeSet] = list(shapes)
        self.labels: list[str] = list(labels) if labels is not None else [""] * len(self.shapes)

    @property
    def count(self) -> int:
        return len(self.shapes)

    def layout(self, settings: LayoutSettings | None = None) -> tuple[list[StrokeSet], object]:
        """把每个图形 ``fit_to_box`` 进一个单元格，返回 (图形列表, Page)。

        按元素数量取接近正方形的网格（``cols ≈ √count``），再交给
        :func:`handwriting.layout.compute` 排版 —— 与文字排版共用同一套版面数学，
        因此预览与机器实际走位必然一致。
        """
        from handwriting.layout import Page

        if settings is None:
            settings = LayoutSettings()
        count = max(1, self.count)
        cols = max(1, round(math.sqrt(count)))
        rows = math.ceil(count / cols)

        grid_settings = LayoutSettings(
            mode=settings.mode,
            page_width_mm=settings.page_width_mm,
            page_height_mm=settings.page_height_mm,
            char_size_mm=settings.char_size_mm,
            chars_per_line=cols,
            line_count=settings.line_count,
            first_line=settings.first_line,
            char_gap_mm=settings.char_gap_mm,
            line_gap_mm=settings.line_gap_mm,
            margin_x_mm=settings.margin_x_mm,
            margin_y_mm=settings.margin_y_mm,
            grid_chars_per_line=cols,
        )
        page: Page = compute([cols] * rows, grid_settings)
        fitted: list[StrokeSet] = []
        for shape in self.shapes:
            fitted.append(shape.fit_to_box(1.0, padding=0.04, stretch=True))
        return fitted, page

    def to_gcode(
        self, plot: PlotSettings | None = None, layout: LayoutSettings | None = None
    ) -> str:
        fitted, page = self.layout(layout)
        return glyphs_to_gcode_with_stats(fitted, page, plot).lines_text()

    def to_svg(self, style: SvgStyle | None = None, layout: LayoutSettings | None = None) -> str:
        fitted, page = self.layout(layout)
        return to_svg(fitted, page, style)


def compose(
    shapes: Sequence[StrokeSet],
    plot: PlotSettings | None = None,
    layout: LayoutSettings | None = None,
    style: SvgStyle | None = None,
) -> tuple[str, str]:
    """便捷入口：多个图形 → (G 代码文本, SVG 预览文本)。"""
    sheet = ShapeSheet(shapes)
    return sheet.to_gcode(plot, layout), sheet.to_svg(style, layout)


__all__ = ["ShapeSheet", "compose"]
