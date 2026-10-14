"""轨迹数据结构与几何变换。

坐标约定（与采集器导出一致）：
    x 向右为正，y 向下为正，取值 0~1 表示归一化到方格内部。
    pressure 取值 0~1，设备不支持压感时恒为 0.5。
    t 为该点相对所在笔画起笔时刻的毫秒数，同一笔内单调不减。
"""

from __future__ import annotations

import math
from collections.abc import Iterator, Sequence
from dataclasses import dataclass, replace
from itertools import pairwise

MIN_POINTS_PER_STROKE = 1


@dataclass(frozen=True, slots=True)
class Point:
    """单个采样点。"""

    x: float
    y: float
    pressure: float = 0.5
    t: int = 0

    def __post_init__(self) -> None:
        if not math.isfinite(self.x) or not math.isfinite(self.y):
            raise ValueError(f"坐标必须为有限数值，收到 ({self.x}, {self.y})")
        if not 0.0 <= self.pressure <= 1.0:
            raise ValueError(f"pressure 必须落在 0~1，收到 {self.pressure}")
        if self.t < 0:
            raise ValueError(f"t 不能为负，收到 {self.t}")

    def as_tuple(self) -> tuple[float, float, float, int]:
        """返回 (x, y, pressure, t)，便于序列化。"""
        return (self.x, self.y, self.pressure, self.t)


@dataclass(slots=True)
class Stroke:
    """一笔。``points`` 至少包含一个采样点。"""

    points: list[Point]

    def __post_init__(self) -> None:
        if len(self.points) < MIN_POINTS_PER_STROKE:
            raise ValueError("Stroke 至少需要一个采样点")

    def __len__(self) -> int:
        return len(self.points)

    def __iter__(self) -> Iterator[Point]:
        return iter(self.points)

    @property
    def length(self) -> float:
        """笔画折线总长度（归一化单位）。"""
        return sum(math.hypot(b.x - a.x, b.y - a.y) for a, b in pairwise(self.points))

    @property
    def duration_ms(self) -> int:
        """书写该笔耗时（毫秒）。"""
        return self.points[-1].t - self.points[0].t

    @property
    def mean_pressure(self) -> float:
        return sum(p.pressure for p in self.points) / len(self.points)

    def bbox(self) -> tuple[float, float, float, float]:
        """返回 (min_x, min_y, max_x, max_y)。"""
        xs = [p.x for p in self.points]
        ys = [p.y for p in self.points]
        return min(xs), min(ys), max(xs), max(ys)

    def translated(self, dx: float, dy: float) -> Stroke:
        return Stroke([replace(p, x=p.x + dx, y=p.y + dy) for p in self.points])

    def scaled(self, sx: float, sy: float, origin: tuple[float, float] = (0.0, 0.0)) -> Stroke:
        """以 ``origin`` 为基点缩放。"""
        ox, oy = origin
        return Stroke(
            [replace(p, x=ox + (p.x - ox) * sx, y=oy + (p.y - oy) * sy) for p in self.points]
        )

    def sheared(self, kx: float = 0.0, ky: float = 0.0) -> Stroke:
        """错切，用于模拟字倾斜度。``kx`` 为水平方向随 y 的倾斜量。"""
        return Stroke([replace(p, x=p.x + kx * p.y, y=p.y + ky * p.x) for p in self.points])


@dataclass(slots=True)
class StrokeSet:
    """一个字的全部笔画，按书写顺序排列 —— 笔顺天然由列表顺序表达。"""

    strokes: list[Stroke]
    char: str = ""

    @property
    def stroke_count(self) -> int:
        return len(self.strokes)

    @property
    def point_count(self) -> int:
        return sum(len(s) for s in self.strokes)

    @property
    def total_length(self) -> float:
        return sum(s.length for s in self.strokes)

    @property
    def duration_ms(self) -> int:
        return sum(s.duration_ms for s in self.strokes)

    def bbox(self) -> tuple[float, float, float, float] | None:
        """所有笔画的联合包围盒，空集返回 None。"""
        if not self.strokes:
            return None
        boxes = [s.bbox() for s in self.strokes]
        return (
            min(b[0] for b in boxes),
            min(b[1] for b in boxes),
            max(b[2] for b in boxes),
            max(b[3] for b in boxes),
        )

    def fit_to_box(self, size: float = 1.0, padding: float = 0.05) -> StrokeSet:
        """等比缩放并居中，使字形恰好装进边长为 ``size`` 的方框。

        等比（而非拉伸）是为了保留书写者的真实字形比例，这是风格特征的一部分。
        """
        box = self.bbox()
        if box is None:
            return StrokeSet([], self.char)

        min_x, min_y, max_x, max_y = box
        width = max_x - min_x
        height = max_y - min_y
        longest = max(width, height)
        if longest <= 0.0:
            return StrokeSet([], self.char)

        target = size * (1.0 - 2.0 * padding)
        factor = target / longest

        # 先把左上角移到原点，再等比缩放，最后在方框内居中
        offset_x = -min_x * factor + (size - width * factor) / 2.0
        offset_y = -min_y * factor + (size - height * factor) / 2.0

        fitted = []
        for stroke in self.strokes:
            pts = [
                replace(p, x=p.x * factor + offset_x, y=p.y * factor + offset_y)
                for p in stroke.points
            ]
            fitted.append(Stroke(pts))
        return StrokeSet(fitted, self.char)

    def flatten(self) -> list[Point]:
        """按书写顺序展开为单一点序列（丢失抬笔信息，仅用于统计或画图）。"""
        return [p for s in self.strokes for p in s.points]


def strokes_from_arrays(raw: Sequence[Sequence[Sequence[float]]]) -> list[Stroke]:
    """从采集器的 ``[[[x, y, p, t], ...], ...]`` 结构构造笔画列表。"""
    strokes: list[Stroke] = []
    for raw_stroke in raw:
        points = [
            Point(x=float(v[0]), y=float(v[1]), pressure=float(v[2]), t=int(v[3]))
            for v in raw_stroke
        ]
        if points:
            strokes.append(Stroke(points))
    return strokes
