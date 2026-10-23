"""笔画 → 「中心线 + 残差」训练样本。

对每一笔：

1. 按弧长重采样到固定点数 ``n_points``；
2. 做滑动平均得到**中心线**（结构）；
3. 真实点减中心线得到**残差**（风格）；
4. 每个点组装一个特征向量，喂给 MDN。

坐标沿用项目约定：归一化 0~1、y 向下。
"""

from __future__ import annotations

import math
from collections.abc import Sequence
from dataclasses import dataclass
from itertools import pairwise

import numpy as np

from handwriting.stroke import Point, Stroke

FEATURE_DIM = 9
"""每点特征：中心线 x/y、切向 x/y、进度、笔画长度、上一残差 x/y、上一笔压。"""

RESIDUAL_SCALE = 50.0
"""残差放大倍数。

真实残差只有 1e-2 量级；直接回归会让 MDN 的 σ 塌缩、梯度爆炸。
训练时把残差（含特征里的上一残差）乘以该系数，生成时再除回去。
"""


@dataclass(slots=True)
class StrokeExample:
    """一笔的训练样本。"""

    features: np.ndarray
    """``(n_points, FEATURE_DIM)`` 的 float32 特征。"""
    residual: np.ndarray
    """``(n_points, 2)`` 的真实残差 (dx, dy)。"""
    pressure: np.ndarray
    """``(n_points,)`` 的真实笔压。"""


def resample_n(points: Sequence[Point], n: int) -> list[Point]:
    """按弧长把一笔重采样为恰好 ``n`` 个点，线性插值 x/y/压感/时间。"""
    if n < 2:
        raise ValueError(f"n 必须 ≥ 2，收到 {n}")
    if not points:
        raise ValueError("笔画不能为空")
    if len(points) == 1:
        p = points[0]
        return [Point(p.x, p.y, p.pressure, p.t) for _ in range(n)]

    dists = [0.0]
    for a, b in pairwise(points):
        dists.append(dists[-1] + math.hypot(b.x - a.x, b.y - a.y))
    total = dists[-1]
    if total <= 0.0:
        p = points[0]
        return [Point(p.x, p.y, p.pressure, p.t) for _ in range(n)]

    out: list[Point] = []
    j = 0
    for i in range(n):
        target = total * i / (n - 1)
        while j < len(points) - 2 and dists[j + 1] < target:
            j += 1
        seg = dists[j + 1] - dists[j]
        r = 0.0 if seg <= 0.0 else (target - dists[j]) / seg
        a, b = points[j], points[j + 1]
        out.append(
            Point(
                a.x + (b.x - a.x) * r,
                a.y + (b.y - a.y) * r,
                a.pressure + (b.pressure - a.pressure) * r,
                int(a.t + (b.t - a.t) * r),
            )
        )
    return out


def smooth_points(points: Sequence[Point], window: int = 5) -> list[Point]:
    """滑动平均，得到去掉细节后的中心线。"""
    if window <= 1 or len(points) < 3:
        return [Point(p.x, p.y, p.pressure, p.t) for p in points]
    half = window // 2
    out: list[Point] = []
    for i, point in enumerate(points):
        lo = max(0, i - half)
        hi = min(len(points), i + half + 1)
        xs = [p.x for p in points[lo:hi]]
        ys = [p.y for p in points[lo:hi]]
        out.append(Point(sum(xs) / len(xs), sum(ys) / len(ys), point.pressure, point.t))
    return out


def _tangent(center: Sequence[Point], i: int) -> tuple[float, float]:
    if i == 0:
        a, b = center[0], center[1]
    elif i == len(center) - 1:
        a, b = center[-2], center[-1]
    else:
        a, b = center[i - 1], center[i + 1]
    tx, ty = b.x - a.x, b.y - a.y
    norm = math.hypot(tx, ty)
    if norm <= 1e-9:
        return 0.0, 0.0
    return tx / norm, ty / norm


def point_feature(
    center: Sequence[Point],
    i: int,
    length_norm: float,
    prev_residual: tuple[float, float],
    prev_pressure: float,
) -> list[float]:
    """组装第 ``i`` 个点的特征。训练与生成共用，保证口径一致。"""
    tx, ty = _tangent(center, i)
    n = len(center)
    s = i / (n - 1) if n > 1 else 0.0
    return [
        center[i].x,
        center[i].y,
        tx,
        ty,
        s,
        length_norm,
        prev_residual[0],
        prev_residual[1],
        prev_pressure,
    ]


def stroke_example(
    stroke: Stroke,
    n_points: int = 48,
    smooth_window: int = 5,
    centerline: Stroke | None = None,
) -> StrokeExample:
    """把一笔转成训练样本。

    ``centerline`` 是「内容」参考（标准骨架 / 合成数据的基准曲线）。
    提供它时残差才是真正的风格；缺省时退化为平滑自身，只能学到局部抖动。
    """
    pts = resample_n(stroke.points, n_points)
    center = (
        resample_n(centerline.points, n_points)
        if centerline is not None
        else smooth_points(pts, smooth_window)
    )

    length = 0.0
    for a, b in pairwise(pts):
        length += math.hypot(b.x - a.x, b.y - a.y)
    length_norm = min(2.0, length)

    features = np.zeros((n_points, FEATURE_DIM), dtype=np.float32)
    residual = np.zeros((n_points, 2), dtype=np.float32)
    pressure = np.zeros((n_points,), dtype=np.float32)

    prev_res = (0.0, 0.0)
    prev_p = 0.5
    for i in range(n_points):
        features[i] = point_feature(center, i, length_norm, prev_res, prev_p)
        dx = (pts[i].x - center[i].x) * RESIDUAL_SCALE
        dy = (pts[i].y - center[i].y) * RESIDUAL_SCALE
        residual[i] = (dx, dy)
        pressure[i] = pts[i].pressure
        prev_res = (dx, dy)
        prev_p = pts[i].pressure

    return StrokeExample(features, residual, pressure)


def build_examples(
    strokes: Sequence[Stroke],
    n_points: int = 48,
    smooth_window: int = 5,
    centerlines: Sequence[Stroke | None] | None = None,
) -> list[StrokeExample]:
    """批量构造样本，跳过点数不足的笔画。"""
    if centerlines is None:
        centerlines = [None] * len(strokes)
    if len(centerlines) != len(strokes):
        raise ValueError(f"centerlines 数量（{len(centerlines)}）与笔画数（{len(strokes)}）不一致")
    return [
        stroke_example(stroke, n_points, smooth_window, center)
        for stroke, center in zip(strokes, centerlines, strict=True)
        if len(stroke.points) >= 2
    ]


def stack(examples: Sequence[StrokeExample]) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """把样本堆成 ``(X, Y_xy, Y_p)`` 三个数组。"""
    if not examples:
        raise ValueError("没有可用的训练样本")
    x = np.stack([e.features for e in examples])
    y_xy = np.stack([e.residual for e in examples])
    y_p = np.stack([e.pressure for e in examples])
    return x, y_xy, y_p
