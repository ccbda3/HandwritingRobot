"""轨迹 → SVG 预览。

用于在不动机器的情况下肉眼验证轨迹：笔顺对不对、字距是否合理、
笔画有没有断裂或粘连、倾斜是否合适。

**预览与 G 代码共用同一套版面计算**（:func:`handwriting.gcode.char_origin`
与 :func:`~handwriting.gcode.page_size`）。若两边各算一遍，预览图就会与机器
实际画出来的位置不一致 —— 而预览的全部意义就是「不动机器也能看到机器会画成什么样」。

笔压用笔画宽度表达，因此预览图同时能反映压感合成是否合理。
"""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass

from handwriting.layout import Page
from handwriting.stroke import Stroke, StrokeSet

DEFAULT_INK = "#141414"
DEFAULT_PAPER = "#ffffff"
DEFAULT_BASE_WIDTH = 0.35
DEFAULT_PRESSURE_WIDTH = 0.75

# 宽度量化级数。把相近的笔压合并成一段，避免每个采样点都生成一个 SVG 元素 ——
# 20 个字约 6000 个采样点，逐段输出会让 SVG 膨胀到几百 KB 且毫无视觉差异。
_WIDTH_LEVELS = 8


@dataclass(slots=True)
class SvgStyle:
    """预览外观参数。宽度单位为 mm，与版面同尺度。"""

    ink: str = DEFAULT_INK
    paper: str = DEFAULT_PAPER
    base_width: float = DEFAULT_BASE_WIDTH
    pressure_width: float = DEFAULT_PRESSURE_WIDTH
    """笔压对线宽的贡献。设为 0 则所有笔画等宽。"""
    show_grid: bool = True
    grid_color: str = "#e8e8e8"
    show_rules: bool = True
    rule_color: str = "#c8d4e0"
    show_stroke_order: bool = False
    order_color: str = "#c0392b"
    decimals: int = 3


def to_svg(
    glyphs: Sequence[StrokeSet],
    page: Page,
    style: SvgStyle | None = None,
) -> str:
    """把字形按版面渲染成 SVG 字符串。

    输出的 viewBox 直接使用毫米坐标，与机床坐标系一一对应，
    因此可以在浏览器里量尺寸、直接对照实际输出。

    与 G 代码生成共用同一个 :class:`~handwriting.layout.Page`，
    因此预览里的位置就是机器实际走的位置。
    """
    look = style if style is not None else SvgStyle()
    width = page.width_mm
    height = page.height_mm

    parts: list[str] = [
        f'<svg xmlns="http://www.w3.org/2000/svg" '
        f'viewBox="0 0 {_n(width, look.decimals)} {_n(height, look.decimals)}" '
        f'width="{_n(width * 4, 1)}" height="{_n(height * 4, 1)}">',
        f'<rect x="0" y="0" width="{_n(width, look.decimals)}" '
        f'height="{_n(height, look.decimals)}" fill="{look.paper}"/>',
    ]

    parts.extend(_rules(page, look))
    parts.extend(_grid(page, look))

    for glyph, cell in zip(glyphs, page.cells, strict=False):
        parts.extend(_render_glyph(glyph, cell.x_mm, cell.y_mm, cell.size_mm, look))

    parts.append("</svg>")
    return "\n".join(parts)


def _render_glyph(
    stroke_set: StrokeSet,
    origin_x: float,
    origin_y: float,
    size: float,
    style: SvgStyle,
) -> list[str]:
    out: list[str] = []

    for order, stroke in enumerate(stroke_set.strokes, start=1):
        out.extend(_render_stroke(stroke, origin_x, origin_y, size, style))

        if style.show_stroke_order and stroke.points:
            start = stroke.points[0]
            out.append(
                f'<text x="{_n(origin_x + start.x * size, 2)}" '
                f'y="{_n(origin_y + start.y * size, 2)}" '
                f'font-size="{_n(size * 0.18, 2)}" fill="{style.order_color}" '
                f'font-family="sans-serif">{order}</text>'
            )
    return out


def _render_stroke(
    stroke: Stroke,
    origin_x: float,
    origin_y: float,
    size: float,
    style: SvgStyle,
) -> list[str]:
    points = stroke.points
    if not points:
        return []

    if len(points) == 1:
        point = points[0]
        radius = _width(point.pressure, style) / 2.0
        return [
            f'<circle cx="{_n(origin_x + point.x * size, 3)}" '
            f'cy="{_n(origin_y + point.y * size, 3)}" r="{_n(radius, 3)}" '
            f'fill="{style.ink}"/>'
        ]

    if style.pressure_width <= 0.0:
        coords = " ".join(
            f"{_n(origin_x + p.x * size, 3)},{_n(origin_y + p.y * size, 3)}" for p in points
        )
        return [
            f'<polyline points="{coords}" fill="none" stroke="{style.ink}" '
            f'stroke-width="{_n(_width(0.5, style), 3)}" '
            f'stroke-linecap="round" stroke-linejoin="round"/>'
        ]

    return _render_pressure_segments(points, origin_x, origin_y, size, style)


def _render_pressure_segments(
    points: Sequence,
    origin_x: float,
    origin_y: float,
    size: float,
    style: SvgStyle,
) -> list[str]:
    """按量化后的笔压把笔画切成若干段，每段一个 polyline。"""
    out: list[str] = []
    segment: list = [points[0]]
    current_level = _level(points[0].pressure)

    for index in range(1, len(points)):
        previous = points[index - 1]
        point = points[index]
        level = _level((previous.pressure + point.pressure) / 2.0)

        if level != current_level and len(segment) > 1:
            out.append(_polyline(segment, current_level, origin_x, origin_y, size, style))
            segment = [previous]

        segment.append(point)
        current_level = level

    if len(segment) > 1:
        out.append(_polyline(segment, current_level, origin_x, origin_y, size, style))
    return out


def _polyline(
    points: Sequence,
    level: int,
    origin_x: float,
    origin_y: float,
    size: float,
    style: SvgStyle,
) -> str:
    coords = " ".join(
        f"{_n(origin_x + p.x * size, 3)},{_n(origin_y + p.y * size, 3)}" for p in points
    )
    width = style.base_width + style.pressure_width * (level / (_WIDTH_LEVELS - 1))
    return (
        f'<polyline points="{coords}" fill="none" stroke="{style.ink}" '
        f'stroke-width="{_n(width, 3)}" stroke-linecap="round" stroke-linejoin="round"/>'
    )


def _level(pressure: float) -> int:
    clamped = min(1.0, max(0.0, pressure))
    return min(_WIDTH_LEVELS - 1, round(clamped * (_WIDTH_LEVELS - 1)))


def _width(pressure: float, style: SvgStyle) -> float:
    clamped = min(1.0, max(0.0, pressure))
    return style.base_width + style.pressure_width * clamped


def _grid(page: Page, style: SvgStyle) -> list[str]:
    """画出每个字的字格，便于检查字距与出格。"""
    if not style.show_grid:
        return []
    out: list[str] = []
    for cell in page.cells:
        out.append(
            f'<rect x="{_n(cell.x_mm, 3)}" y="{_n(cell.y_mm, 3)}" '
            f'width="{_n(cell.size_mm, 3)}" height="{_n(cell.size_mm, 3)}" '
            f'fill="none" stroke="{style.grid_color}" stroke-width="0.1"/>'
        )
    return out


def _rules(page: Page, style: SvgStyle) -> list[str]:
    """画出横条本的横线。字应当压在这些线上。"""
    if not style.show_rules or not page.rule_lines_mm:
        return []
    out: list[str] = []
    for y_mm in page.rule_lines_mm:
        out.append(
            f'<line x1="0" y1="{_n(y_mm, 3)}" x2="{_n(page.width_mm, 3)}" '
            f'y2="{_n(y_mm, 3)}" stroke="{style.rule_color}" stroke-width="0.15"/>'
        )
    return out


def _n(value: float, decimals: int) -> str:
    """格式化数值：去掉无意义的尾随零，减小 SVG 体积。"""
    text = f"{value:.{decimals}f}"
    if "." in text:
        text = text.rstrip("0").rstrip(".")
    return text or "0"
