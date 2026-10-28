"""骨架对齐：把采集笔画映射到标准字形骨架。

真实数据训练的前提。没有对齐时只能拿「平滑自身」当中心线，全局倾斜、比例、
字距会被吸收进中心线，模型只学得到抖动；对齐之后，残差才是**你的笔画风格**。

做法：

1. 用整体 bbox 拟合一个**等比缩放 + 平移**（不旋转），把标准骨架搬到采集字的位置；
2. 按几何代价贪心匹配笔画（允许笔画数不一致）；
3. 方向校正：如果反向更接近，就反转标准骨架（保留用户的书写方向）；
4. 两侧都重采样到固定点数，交给 ``stroke_example`` 生成训练样本。
"""

from __future__ import annotations

import math
from collections.abc import Sequence
from dataclasses import dataclass

from handwriting.model.data import StrokeExample, resample_n, stroke_example
from handwriting.stroke import Point, Stroke, StrokeSet

_EPS = 1e-9


@dataclass(slots=True)
class AlignedStroke:
    """一对对齐后的笔画。"""

    reference: Stroke
    """标准骨架（已变换到采集字的位置）。"""
    target: Stroke
    """用户的真实笔画。"""
    cost: float
    """匹配代价，越小越可信。"""
    reference_index: int
    """在标准骨架里的笔画序号，便于按书写顺序排序。"""


def _set_bbox(stroke_set: StrokeSet) -> tuple[float, float, float, float]:
    xs = [p.x for stroke in stroke_set.strokes for p in stroke.points]
    ys = [p.y for stroke in stroke_set.strokes for p in stroke.points]
    return min(xs), min(ys), max(xs), max(ys)


def _fit_transform(
    reference: StrokeSet, target: StrokeSet
) -> tuple[float, float, float, float, float]:
    """返回 (scale, src_cx, src_cy, dst_cx, dst_cy)：把 reference 搬到 target。"""
    rx0, ry0, rx1, ry1 = _set_bbox(reference)
    tx0, ty0, tx1, ty1 = _set_bbox(target)
    rw = max(_EPS, rx1 - rx0)
    rh = max(_EPS, ry1 - ry0)
    tw = max(_EPS, tx1 - tx0)
    th = max(_EPS, ty1 - ty0)

    # 等比缩放：只统一比例，宽高比差异留给残差（那正是风格）
    if rx1 - rx0 <= 1e-6 and ry1 - ry0 <= 1e-6:
        scale = 1.0
    elif rx1 - rx0 <= 1e-6:
        scale = th / rh
    elif ry1 - ry0 <= 1e-6:
        scale = tw / rw
    else:
        scale = min(tw / rw, th / rh)

    return scale, (rx0 + rx1) / 2.0, (ry0 + ry1) / 2.0, (tx0 + tx1) / 2.0, (ty0 + ty1) / 2.0


def _transform_stroke(
    stroke: Stroke, scale: float, scx: float, scy: float, dcx: float, dcy: float
) -> Stroke:
    return Stroke(
        [
            Point(
                (p.x - scx) * scale + dcx,
                (p.y - scy) * scale + dcy,
                p.pressure,
                p.t,
            )
            for p in stroke.points
        ]
    )


def _reverse(stroke: Stroke) -> Stroke:
    return Stroke(list(reversed(stroke.points)))


def _endpoint_cost(a: Stroke, b: Stroke, *, reverse: bool = False) -> float:
    ap = list(reversed(a.points)) if reverse else a.points
    return math.hypot(ap[0].x - b.points[0].x, ap[0].y - b.points[0].y) + math.hypot(
        ap[-1].x - b.points[-1].x, ap[-1].y - b.points[-1].y
    )


def _stroke_cost(a: Stroke, b: Stroke) -> float:
    direct = _endpoint_cost(a, b, reverse=False)
    reversed_cost = _endpoint_cost(a, b, reverse=True)
    return min(direct, reversed_cost) + 0.5 * abs(a.length - b.length)


def align_stroke_sets(
    captured: StrokeSet,
    reference: StrokeSet,
    n_points: int = 48,
    min_cost: float | None = None,
) -> list[AlignedStroke]:
    """把标准骨架 ``reference`` 对齐到采集字 ``captured``。

    ``min_cost`` 是匹配代价上限；超过的笔画对会被跳过（不参与训练），
    用来过滤写坏/多写/少写的样本。``None`` 表示不过滤。
    """
    if not captured.strokes or not reference.strokes:
        return []

    scale, scx, scy, dcx, dcy = _fit_transform(reference, captured)
    refs = [_transform_stroke(stroke, scale, scx, scy, dcx, dcy) for stroke in reference.strokes]
    caps = list(captured.strokes)

    candidates = [
        (_stroke_cost(ref, cap), i, j) for i, ref in enumerate(refs) for j, cap in enumerate(caps)
    ]
    candidates.sort(key=lambda item: item[0])

    used_ref: set[int] = set()
    used_cap: set[int] = set()
    aligned: list[AlignedStroke] = []
    for cost, i, j in candidates:
        if i in used_ref or j in used_cap:
            continue
        if min_cost is not None and cost > min_cost:
            continue
        used_ref.add(i)
        used_cap.add(j)
        ref = refs[i]
        cap = caps[j]
        if _endpoint_cost(ref, cap, reverse=False) > _endpoint_cost(ref, cap, reverse=True):
            ref = _reverse(ref)
        aligned.append(
            AlignedStroke(
                reference=Stroke(resample_n(ref.points, n_points)),
                target=Stroke(resample_n(cap.points, n_points)),
                cost=cost,
                reference_index=i,
            )
        )

    aligned.sort(key=lambda item: item.reference_index)
    return aligned


def build_aligned_examples(
    pairs: Sequence[tuple[StrokeSet, StrokeSet]],
    n_points: int = 48,
    smooth_window: int = 5,
    min_cost: float | None = None,
) -> list[StrokeExample]:
    """``[(采集字, 标准骨架), ...]`` → 训练样本（中心线 = 标准骨架）。"""
    examples: list[StrokeExample] = []
    for captured, reference in pairs:
        for aligned in align_stroke_sets(captured, reference, n_points, min_cost):
            examples.append(
                stroke_example(
                    aligned.target,
                    n_points=n_points,
                    smooth_window=smooth_window,
                    centerline=aligned.reference,
                )
            )
    return examples
