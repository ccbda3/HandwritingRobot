"""版面计算 —— 字格从哪来。

两种模式：

``Grid``
    通用排版：固定字格 + 字距 + 行距 + 页边距。用于导到空白纸。
``Notebook``
    横条本（作业本）：字格按页宽与行数均分，**字压在横线上**。
    默认幅面 150×213 mm / 22 行 —— 常见作业本去掉页边距后的书写区。

与 C++ 侧 ``core/src/layout.cpp`` 行为等价，两侧必须同步修改，
否则命令行导出的 G 代码与 Qt 预览会不一致。

关键点：字形在字格内的位置由**基线**决定，不是由字格顶部决定。
字形数据里基线固定在归一化坐标的 0.8789 处（见
:data:`handwriting.glyph.BASELINE_RATIO`），因此「字压在横线上」要反推
字格顶部：``top = baseline - BASELINE_RATIO × char_size``。
"""

from __future__ import annotations

import math
from collections.abc import Sequence
from dataclasses import dataclass, field
from enum import Enum

from handwriting.glyph import BASELINE_RATIO

DEFAULT_GRID_CHAR_SIZE = 20.0

# 常见作业本（横条本）去掉页边距后的书写区
NOTEBOOK_WIDTH_MM = 150.0
NOTEBOOK_HEIGHT_MM = 213.0
NOTEBOOK_LINES = 22


class Mode(Enum):
    GRID = "grid"
    NOTEBOOK = "notebook"


@dataclass(slots=True)
class LayoutSettings:
    """版面参数。所有长度单位为毫米。"""

    mode: Mode = Mode.NOTEBOOK

    # 幅面
    page_width_mm: float = NOTEBOOK_WIDTH_MM
    page_height_mm: float = NOTEBOOK_HEIGHT_MM

    # 字格。0 表示自动：Notebook 取行距，Grid 取 20mm
    char_size_mm: float = 0.0
    # 0 表示自动：按页宽和字宽算出能放几个
    chars_per_line: int = 0

    # Notebook 模式
    line_count: int = NOTEBOOK_LINES
    first_line: int = 1
    """从第几行开始写（1 起）。留白或避开本子上印刷的标题时用。"""

    # Grid 模式
    char_gap_mm: float = 3.0
    line_gap_mm: float = 10.0
    margin_x_mm: float = 10.0
    margin_y_mm: float = 10.0
    grid_chars_per_line: int = 10


@dataclass(frozen=True, slots=True)
class Cell:
    """一个字的字格。"""

    x_mm: float
    y_mm: float
    size_mm: float
    baseline_mm: float
    line: int
    column: int


@dataclass(slots=True)
class Page:
    width_mm: float
    height_mm: float
    chars_per_line: int
    cells: list[Cell] = field(default_factory=list)
    rule_lines_mm: list[float] = field(default_factory=list)
    """横线的 y 坐标，仅用于绘制预览。"""


def notebook_pitch(settings: LayoutSettings) -> float:
    if settings.line_count < 1:
        raise ValueError(f"line_count 必须 ≥ 1，收到 {settings.line_count}")
    return settings.page_height_mm / settings.line_count


def resolve_char_size(settings: LayoutSettings) -> float:
    if settings.char_size_mm > 0.0:
        return settings.char_size_mm
    if settings.mode is Mode.NOTEBOOK:
        # 默认字宽 = 行距：字刚好占满一行，符合作业本上写汉字的观感
        return notebook_pitch(settings)
    return DEFAULT_GRID_CHAR_SIZE


def resolve_chars_per_line(settings: LayoutSettings) -> int:
    if settings.chars_per_line > 0:
        return settings.chars_per_line
    if settings.mode is Mode.GRID:
        return max(1, settings.grid_chars_per_line)
    size = resolve_char_size(settings)
    if size <= 0.0:
        return 1
    return max(1, math.floor(settings.page_width_mm / size))


def compute(row_char_counts: Sequence[int], settings: LayoutSettings) -> Page:
    """计算版面。

    ``row_char_counts`` 为每一行的字数（文本里的显式换行产生新行）。
    超出 ``chars_per_line`` 的行会自动折到下一行。
    """
    char_size = resolve_char_size(settings)
    if char_size <= 0.0:
        raise ValueError("字宽必须为正")
    per_line = resolve_chars_per_line(settings)

    if settings.mode is Mode.NOTEBOOK:
        pitch = notebook_pitch(settings)
        cell_width = settings.page_width_mm / per_line
        page = Page(
            width_mm=settings.page_width_mm,
            height_mm=settings.page_height_mm,
            chars_per_line=per_line,
            rule_lines_mm=[(i + 1) * pitch for i in range(settings.line_count)],
        )

        line = max(0, settings.first_line - 1)
        for row_count in row_char_counts:
            column = 0
            for _ in range(row_count):
                if column >= per_line:
                    line += 1
                    column = 0
                baseline = (line + 1) * pitch
                page.cells.append(
                    Cell(
                        x_mm=column * cell_width + (cell_width - char_size) / 2.0,
                        y_mm=baseline - BASELINE_RATIO * char_size,
                        size_mm=char_size,
                        baseline_mm=baseline,
                        line=line,
                        column=column,
                    )
                )
                column += 1
            line += 1  # 显式换行后必定另起一行
        return page

    # Grid 模式
    total = sum(row_char_counts)
    width = (
        settings.margin_x_mm * 2.0
        + per_line * char_size
        + max(0, per_line - 1) * settings.char_gap_mm
    )

    page = Page(width_mm=width, height_mm=0.0, chars_per_line=per_line)
    line = 0
    for row_count in row_char_counts:
        column = 0
        for _ in range(row_count):
            if column >= per_line:
                line += 1
                column = 0
            x = settings.margin_x_mm + column * (char_size + settings.char_gap_mm)
            y = settings.margin_y_mm + line * (char_size + settings.line_gap_mm)
            page.cells.append(
                Cell(
                    x_mm=x,
                    y_mm=y,
                    size_mm=char_size,
                    baseline_mm=y + BASELINE_RATIO * char_size,
                    line=line,
                    column=column,
                )
            )
            column += 1
        line += 1

    used_lines = 0 if total == 0 else max(1, line)
    page.height_mm = (
        settings.margin_y_mm * 2.0
        + used_lines * char_size
        + max(0, used_lines - 1) * settings.line_gap_mm
    )
    return page


def compute_uniform(char_count: int, settings: LayoutSettings) -> Page:
    """便捷入口：单个连续段落。"""
    per_line = resolve_chars_per_line(settings)
    rows: list[int] = []
    remaining = char_count
    while remaining > 0:
        take = min(remaining, per_line)
        rows.append(take)
        remaining -= take
    if not rows:
        rows.append(0)
    return compute(rows, settings)
