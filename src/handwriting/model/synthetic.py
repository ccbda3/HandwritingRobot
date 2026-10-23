"""合成笔迹：没有数位板时用来跑通训练与生成。

关键点：同时返回**基准曲线**（内容）和**加风格后的曲线**（真实笔迹）。
训练时用基准曲线当中心线，残差里才会包含倾斜、弧形、笔压这些「风格」；
若用平滑后的真实笔迹当中心线，全局倾斜会被吸收进中心线，模型就学不到。
"""

from __future__ import annotations

import math

import numpy as np

from handwriting.stroke import Point, Stroke


def _base_curve(kind: int, t: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    if kind == 0:  # 近水平
        return t, 0.5 + 0.15 * (t - 0.5)
    if kind == 1:  # 对角
        return t, t
    if kind == 2:  # 弧形
        return t, 0.5 + 0.25 * np.sin(np.pi * t)
    if kind == 3:  # S 形
        return t, 0.5 + 0.18 * np.sin(2.0 * np.pi * t)
    # 近竖直
    return 0.5 + 0.15 * (t - 0.5), t


def _to_stroke(x: np.ndarray, y: np.ndarray, pressure: np.ndarray) -> Stroke:
    return Stroke(
        [Point(float(x[i]), float(y[i]), float(pressure[i]), i * 10) for i in range(len(x))]
    )


def synthetic_pairs(
    count: int = 240,
    n_points: int = 48,
    seed: int = 0,
    slant: float = 0.35,
    bow: float = 0.05,
    pressure_amp: float = 0.25,
    noise: float = 0.006,
) -> list[tuple[Stroke, Stroke]]:
    """返回 ``[(基准曲线, 加风格曲线), ...]``。"""
    rng = np.random.default_rng(seed)
    t = np.linspace(0.0, 1.0, n_points)
    pairs: list[tuple[Stroke, Stroke]] = []

    for _ in range(count):
        kind = int(rng.integers(0, 5))
        x, y = _base_curve(kind, t)

        # 内容：缩放进 0.1~0.9
        x = 0.1 + 0.8 * x
        y = 0.1 + 0.8 * y
        base = _to_stroke(x, y, np.full(n_points, 0.5))

        # 风格：倾斜 + 弧形 + 两端重压 + 噪声
        sx = x + slant * (y - 0.5) + rng.normal(0.0, noise, n_points)
        sy = y + bow * np.sin(np.pi * t) + rng.normal(0.0, noise, n_points)
        pressure = 0.55 + pressure_amp * (1.0 - np.sin(np.pi * t))
        pressure = np.clip(pressure + rng.normal(0.0, 0.02, n_points), 0.0, 1.0)
        styled = _to_stroke(sx, sy, pressure)

        pairs.append((base, styled))

    return pairs


def synthetic_strokes(
    count: int = 240, n_points: int = 48, seed: int = 0, **kwargs
) -> list[Stroke]:
    """只要加风格后的曲线（兼容旧调用）。"""
    return [styled for _, styled in synthetic_pairs(count, n_points, seed, **kwargs)]


def glyph_pairs(
    chars: str,
    source: object | None = None,
    n_points: int = 48,
    variants: int = 8,
    seed: int = 0,
    slant: float = 0.18,
    bow: float = 0.02,
    pressure_amp: float = 0.20,
    noise: float = 0.003,
) -> list[tuple[Stroke, Stroke]]:
    """用真实字形骨架当基准，叠加合成风格，返回 ``[(基准, 加风格), ...]``。

    比随机曲线更接近真实笔画分布：长短、曲率、转折都来自字形库。
    """
    from handwriting.glyph import HanziWriterSource
    from handwriting.model.data import resample_n

    src = source if source is not None else HanziWriterSource(offline=True)
    rng = np.random.default_rng(seed)
    t = np.linspace(0.0, 1.0, n_points)
    pairs: list[tuple[Stroke, Stroke]] = []

    for char in chars:
        try:
            glyph = src.load(char)  # type: ignore[attr-defined]
        except (LookupError, OSError, ValueError):
            continue
        stroke_set = glyph.to_stroke_set()
        for stroke in stroke_set.strokes:
            if len(stroke.points) < 2:
                continue
            base_points = resample_n(stroke.points, n_points)
            base = Stroke([Point(p.x, p.y, 0.5, p.t) for p in base_points])
            bx = np.array([p.x for p in base_points])
            by = np.array([p.y for p in base_points])

            for _ in range(variants):
                s = slant * (0.8 + 0.4 * rng.random())
                b = bow * (0.8 + 0.4 * rng.random())
                amp = pressure_amp * (0.8 + 0.4 * rng.random())
                sx = bx + s * (by - 0.5) + rng.normal(0.0, noise, n_points)
                sy = by + b * np.sin(np.pi * t) + rng.normal(0.0, noise, n_points)
                pressure = 0.55 + amp * (1.0 - np.sin(np.pi * t))
                pressure = np.clip(pressure + rng.normal(0.0, 0.015, n_points), 0.0, 1.0)
                styled = Stroke(
                    [
                        Point(float(sx[i]), float(sy[i]), float(pressure[i]), i * 10)
                        for i in range(n_points)
                    ]
                )
                pairs.append((base, styled))

    return pairs


def make_straight_stroke(n_points: int = 48) -> Stroke:
    """一条干净的水平线，用于观察模型加上了什么风格。"""
    t = np.linspace(0.0, 1.0, n_points)
    return Stroke([Point(0.1 + 0.8 * float(t[i]), 0.5, 0.5, i * 10) for i in range(n_points)])


def _sanity() -> None:
    pairs = synthetic_pairs(count=3, n_points=16)
    assert len(pairs) == 3
    assert all(len(s.points) == 16 for _, s in pairs)
    assert math.isfinite(pairs[0][1].points[0].x)


if __name__ == "__main__":  # pragma: no cover
    _sanity()
