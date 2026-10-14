"""曲线插值与重采样。

两类需求归结为同一件事——在点序列上做几何处理：

- **字形数据**（Make Me a Hanzi 的 medians）是稀疏折线，直接连线会有明显折角，
  需要样条插值成平滑曲线
- **采集数据**（每笔几十到几百个点）需要在分析前统一采样密度，
  否则 60 Hz 与 240 Hz 设备的特征不可比

本模块只操作 ``Point`` 序列，不涉及 ``Stroke`` 容器，供 :mod:`handwriting.glyph`
与 :mod:`handwriting.style` 共用。
"""

from __future__ import annotations

from collections.abc import Sequence

from handwriting.stroke import Point

_EPS = 1e-12

# 低于该长度的线段不参与重采样步进，其方向由浮点噪声主导
_MIN_SEGMENT = 1e-6


def resample(points: Sequence[Point], step: float) -> list[Point]:
    """按固定弧长重采样，压力与时间线性插值。

    末段短于 :data:`_MIN_SEGMENT` 时不再补端点：浮点累积（如 ``25 × 0.02 ≠ 0.5``）
    会产生长度接近 0 的末段，其方向由噪声主导，会让下游的转折角计算完全错误。
    """
    if step <= 0.0:
        raise ValueError(f"step 必须为正，收到 {step}")
    if len(points) < 2:
        return list(points)

    out: list[Point] = [points[0]]
    carry = 0.0

    for index in range(len(points) - 1):
        a = points[index]
        b = points[index + 1]
        seg = ((b.x - a.x) ** 2 + (b.y - a.y) ** 2) ** 0.5
        if seg <= _EPS:
            continue

        travelled = 0.0
        while carry + (seg - travelled) >= step:
            travelled += step - carry
            out.append(_lerp(a, b, travelled / seg))
            carry = 0.0
        carry += seg - travelled

    last = points[-1]
    if carry > _MIN_SEGMENT and (out[-1].x != last.x or out[-1].y != last.y):
        out.append(last)
    return out


def catmull_rom(points: Sequence[Point], samples_per_segment: int = 12) -> list[Point]:
    """Catmull-Rom 样条插值，把稀疏折线变成平滑曲线。

    选 Catmull-Rom 而不是 B 样条：它**通过所有控制点**。
    字形中心线的控制点本身落在笔画上，被平滑掉就意味着笔画走偏。

    端点采用重复法处理（首段令 P0 = P1），曲线端点因此不会外扩。
    """
    if samples_per_segment < 1:
        raise ValueError(f"samples_per_segment 必须 ≥ 1，收到 {samples_per_segment}")

    count = len(points)
    if count < 2:
        return list(points)

    out: list[Point] = []
    for i in range(count - 1):
        p0 = points[i - 1] if i > 0 else points[i]
        p1 = points[i]
        p2 = points[i + 1]
        p3 = points[i + 2] if i + 2 < count else points[i + 1]

        is_last = i == count - 2
        # 非末段不取 t=1，避免与下一段的起点重复；末段必须取到 t=1 才算完整
        steps = samples_per_segment + (1 if is_last else 0)
        for s in range(steps):
            out.append(_catmull(p0, p1, p2, p3, s / samples_per_segment))
    return out


def smooth_and_resample(
    points: Sequence[Point],
    step: float,
    samples_per_segment: int = 12,
) -> list[Point]:
    """先样条插值再按弧长重采样，得到间距均匀的平滑点序列。

    顺序不能颠倒：先重采样会让稀疏控制点之间的直线段被固化，
    样条插值再平滑也救不回来。
    """
    if len(points) < 2:
        return list(points)
    return resample(catmull_rom(points, samples_per_segment), step)


def _lerp(a: Point, b: Point, ratio: float) -> Point:
    return Point(
        a.x + (b.x - a.x) * ratio,
        a.y + (b.y - a.y) * ratio,
        a.pressure + (b.pressure - a.pressure) * ratio,
        int(a.t + (b.t - a.t) * ratio),
    )


def _catmull(p0: Point, p1: Point, p2: Point, p3: Point, t: float) -> Point:
    t2 = t * t
    t3 = t2 * t

    def axis(a: float, b: float, c: float, d: float) -> float:
        return 0.5 * (
            2.0 * b
            + (-a + c) * t
            + (2.0 * a - 5.0 * b + 4.0 * c - d) * t2
            + (-a + 3.0 * b - 3.0 * c + d) * t3
        )

    return Point(
        axis(p0.x, p1.x, p2.x, p3.x),
        axis(p0.y, p1.y, p2.y, p3.y),
        p1.pressure + (p2.pressure - p1.pressure) * t,
        int(p1.t + (p2.t - p1.t) * t),
    )
