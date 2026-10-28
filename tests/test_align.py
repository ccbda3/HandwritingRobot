"""骨架对齐测试：变换、方向、笔画数不一致、训练样本生成。"""

from __future__ import annotations

import math

import numpy as np

from handwriting.model.align import align_stroke_sets, build_aligned_examples
from handwriting.model.data import FEATURE_DIM
from handwriting.stroke import Point, Stroke, StrokeSet


def _stroke(points: list[tuple[float, float]]) -> Stroke:
    return Stroke([Point(x, y, 0.5, i * 10) for i, (x, y) in enumerate(points)])


def _reference() -> StrokeSet:
    return StrokeSet(
        [
            _stroke([(0.2, 0.2), (0.8, 0.2)]),
            _stroke([(0.5, 0.2), (0.5, 0.8)]),
        ],
        "十",
    )


def _transform(stroke_set: StrokeSet, scale: float, dx: float, dy: float) -> StrokeSet:
    return StrokeSet(
        [
            Stroke(
                [Point(p.x * scale + dx, p.y * scale + dy, p.pressure, p.t) for p in stroke.points]
            )
            for stroke in stroke_set.strokes
        ],
        stroke_set.char,
    )


def _max_point_distance(a: Stroke, b: Stroke) -> float:
    return max(math.hypot(p.x - q.x, p.y - q.y) for p, q in zip(a.points, b.points, strict=True))


def test_identity_alignment_has_near_zero_residual() -> None:
    reference = _reference()
    aligned = align_stroke_sets(reference, reference, n_points=24)
    assert len(aligned) == 2
    for pair in aligned:
        assert _max_point_distance(pair.reference, pair.target) < 1e-9
        assert pair.cost < 1e-9


def test_scale_and_translate_are_removed() -> None:
    reference = _reference()
    captured = _transform(reference, 0.6, 0.2, 0.1)
    aligned = align_stroke_sets(captured, reference, n_points=24)
    assert len(aligned) == 2
    for pair in aligned:
        assert _max_point_distance(pair.reference, pair.target) < 1e-9


def test_reversed_stroke_is_corrected() -> None:
    reference = _reference()
    captured = StrokeSet(
        [
            Stroke(list(reversed(reference.strokes[0].points))),
            reference.strokes[1],
        ],
        "十",
    )
    aligned = align_stroke_sets(captured, reference, n_points=24)
    assert len(aligned) == 2
    first = aligned[0]
    assert _max_point_distance(first.reference, first.target) < 1e-9


def test_extra_stroke_is_ignored() -> None:
    reference = _reference()
    captured = StrokeSet(
        [
            reference.strokes[0],
            reference.strokes[1],
            _stroke([(0.1, 0.9), (0.2, 0.9)]),
        ],
        "十",
    )
    aligned = align_stroke_sets(captured, reference, n_points=24)
    assert len(aligned) == 2


def test_min_cost_filters_bad_pairs() -> None:
    reference = StrokeSet(
        [
            _stroke([(0.2, 0.2), (0.5, 0.2)]),
            _stroke([(0.5, 0.2), (0.8, 0.8)]),
        ],
        "十",
    )
    captured = StrokeSet(
        [reference.strokes[0], _stroke([(0.8, 0.2), (0.8, 0.8)])],
        "十",
    )
    all_pairs = align_stroke_sets(captured, reference, n_points=24)
    filtered = align_stroke_sets(captured, reference, n_points=24, min_cost=0.01)
    assert len(all_pairs) == 2
    assert len(filtered) == 1


def test_build_aligned_examples_uses_reference_as_centerline() -> None:
    reference = _reference()
    captured = _transform(reference, 0.8, 0.1, 0.05)
    examples = build_aligned_examples([(captured, reference)], n_points=24)
    assert len(examples) == 2
    assert examples[0].features.shape == (24, FEATURE_DIM)
    # 已经对齐，残差应当接近 0（RESIDUAL_SCALE 放大后仍然很小）
    assert float(np.abs(examples[0].residual).max()) < 1e-3
