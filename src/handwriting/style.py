"""笔迹风格特征提取。

用于三个目的：

1. **主动选字** —— 度量已采集样本的风格覆盖度，找出缺口（见 curriculum.py）
2. **生成质量评估** —— 把生成结果与用户真实笔迹的特征向量做比较，得到可复现的数值指标
3. **风格条件** —— 为模型提供显式的风格向量，而不只依赖隐式的 style reference

设计约定：

- 角度单位为度，倾斜角**右倾为正**
- 速度单位为「归一化坐标 / 秒」。所有样本都在同一个 0~1 方框内书写，因此可直接比较
- 特征**不做 0~1 归一化**，保留量纲与正负号；比较时用 :func:`distance` 或 z-score
- 压感相关特征在设备不支持压感时为 ``None``，而非 0 —— 两者含义完全不同
- 节奏特征（``pause_ratio``）需要采集数据提供每笔的绝对起始时刻，缺失时为 ``None``

性能说明：本模块目前用 Python 实现，服务于研究期的参数搜索与验证。
若将来需要在前端实时计算（如拖动参数时刷新覆盖度），按 ADR-0003 移植到 ``core/``。
"""

from __future__ import annotations

import math
from collections.abc import Iterable, Sequence
from dataclasses import dataclass, fields
from itertools import pairwise

from handwriting import curve
from handwriting.stroke import Stroke, StrokeSet

DEFAULT_RESAMPLE_STEP = 0.02

# 超过该角度的顶点视为「显著转折」（即楷书里的折）。
# 阈值取 30° 是因为：平滑曲线（撇、捺）经重采样后单个顶点转角远小于此，
# 而真正的折（如「口」的右上角）在重采样后仍保留接近 90° 的单个顶点。
CORNER_THRESHOLD_DEG = 30.0

_EPS = 1e-12

# 参与角度计算的最短线段。低于该长度的线段方向由浮点噪声主导，
# 算出的转折角毫无意义，必须丢弃而不是当作一个「尖角」。
_MIN_SEGMENT = 1e-6

# 各特征的归一化尺度，用于把不同量纲的特征放进同一个距离度量。
# 数值为「一个典型书写者之间的差异量」，**属于初始估计，需用真实数据标定**。
_SCALE: dict[str, float] = {
    "aspect_ratio": 0.5,
    "slant_deg": 10.0,
    "ink_density": 0.5,
    "straightness": 0.2,
    "turn_mean_deg": 5.0,
    "corner_deg": 20.0,
    "corner_rate": 1.0,
    "stroke_length_cv": 0.3,
    "pressure_mean": 0.15,
    "pressure_std": 0.1,
    "pressure_taper": 0.1,
    "speed_mean": 0.5,
    "speed_std": 0.3,
    "pause_ratio": 0.15,
}


@dataclass(frozen=True, slots=True)
class StyleFeatures:
    """一个字形（一个字）的风格特征向量。"""

    # —— 结构 ——
    stroke_count: int
    aspect_ratio: float
    """包围盒宽 / 高。反映字形是偏瘦长还是偏扁平。"""
    slant_deg: float
    """主倾角，右倾为正。由所有偏竖直笔段加权平均得到。"""
    ink_density: float
    """总笔画长度 / 包围盒对角线。反映字写得紧凑还是舒展。"""
    straightness: float
    """各笔端点直线距离之和 / 折线长度之和。1 表示全是直线，越小越曲折。"""
    turn_mean_deg: float
    """所有顶点的平均转折角。反映整体平滑度：行书偏大，楷书偏小。"""
    corner_deg: float
    """显著转折（>30°）的平均角度，即「折」有多尖锐。无显著转折时为 0。"""
    corner_rate: float
    """显著转折数 / 总笔画长度。反映字里折笔的密度。"""
    stroke_length_cv: float
    """笔画长度变异系数（标准差 / 均值）。反映长短笔的对比强度。"""

    # —— 压力 ——
    pressure_mean: float | None
    pressure_std: float | None
    pressure_taper: float | None
    """笔画中段压力 − 首尾压力。正值表示中段重、两头轻（提按明显）。"""

    # —— 节奏 ——
    speed_mean: float | None
    speed_std: float | None
    pause_ratio: float | None
    """抬笔时间 / 总书写时间。反映书写是连贯还是字字停顿。"""

    @property
    def numeric_fields(self) -> tuple[str, ...]:
        return tuple(f.name for f in fields(self) if f.name != "stroke_count")

    def to_vector(self) -> tuple[float, ...]:
        """展开为定长向量，``None`` 以 0 填充。配合 :meth:`availability` 使用。"""
        return tuple(
            0.0 if (v := getattr(self, name)) is None else float(v) for name in self.numeric_fields
        )

    def availability(self) -> tuple[bool, ...]:
        """与 :meth:`to_vector` 等长的可用性掩码，避免把「无压感」误当成「压力为 0」。"""
        return tuple(getattr(self, name) is not None for name in self.numeric_fields)


def resample(stroke: Stroke, step: float = DEFAULT_RESAMPLE_STEP) -> Stroke:
    """按固定弧长重采样，消除采样率差异对角度类特征的影响。

    不同设备的采样率可能相差三倍以上（60 Hz 到 240 Hz），
    直接在原始点上算转折角会让高采样率设备的噪声被放大。

    几何处理委托给 :func:`handwriting.curve.resample`，本函数只负责
    ``Stroke`` 容器的包装。
    """
    return Stroke(curve.resample(stroke.points, step))


def _slant_deg(strokes: Sequence[Stroke]) -> float:
    """主倾角，**右倾为正**。只统计偏竖直的笔段，按竖直长度加权。

    符号约定容易搞反，这里写清楚：右倾的字（像斜体）竖直笔画是
    「顶偏右、底偏左」，沿笔画向下 dx < 0、dy > 0。
    而 ``atan2(dx, dy)`` 在这种情况下给出负值，因此需要取反，
    才能让「右倾为正」成立。
    """
    numerator = 0.0
    denominator = 0.0
    for stroke in strokes:
        for a, b in pairwise(stroke.points):
            dx = b.x - a.x
            dy = b.y - a.y
            if abs(dy) <= abs(dx):
                continue
            weight = abs(dy)
            numerator += -math.degrees(math.atan2(dx, dy)) * weight
            denominator += weight
    return numerator / denominator if denominator > _EPS else 0.0


def _turn_angles_deg(stroke: Stroke) -> list[float]:
    """逐顶点转折角。0 为笔直，180 为原路折返。"""
    angles: list[float] = []
    points = stroke.points
    for i in range(1, len(points) - 1):
        ax = points[i].x - points[i - 1].x
        ay = points[i].y - points[i - 1].y
        bx = points[i + 1].x - points[i].x
        by = points[i + 1].y - points[i].y
        na = math.hypot(ax, ay)
        nb = math.hypot(bx, by)
        if na <= _MIN_SEGMENT or nb <= _MIN_SEGMENT:
            continue
        cosine = max(-1.0, min(1.0, (ax * bx + ay * by) / (na * nb)))
        angles.append(math.degrees(math.acos(cosine)))
    return angles


def _corner_stats(turns: Sequence[float], total_length: float) -> tuple[float, float]:
    """返回（显著转折的平均角, 显著转折密度）。

    不用分位数：一个 90° 的折混在几十个接近 0° 的顶点里，
    90 分位取到的仍是 0，会把最关键的「折笔」信息丢掉。
    改用阈值筛选，直接度量「折有多尖」和「折有多少」。
    """
    corners = [angle for angle in turns if angle > CORNER_THRESHOLD_DEG]
    if not corners:
        return 0.0, 0.0
    mean_deg = sum(corners) / len(corners)
    rate = len(corners) / total_length if total_length > _EPS else 0.0
    return mean_deg, rate


def _pressure_taper(strokes: Sequence[Stroke]) -> float | None:
    """中段压力 − 首尾压力。样本不足时返回 None。"""
    tapers: list[float] = []
    for stroke in strokes:
        points = stroke.points
        if len(points) < 5:
            continue
        edge = max(1, len(points) // 5)
        head = points[:edge]
        tail = points[-edge:]
        middle = points[edge:-edge]
        if not middle:
            continue
        edge_mean = (sum(p.pressure for p in head) + sum(p.pressure for p in tail)) / (
            len(head) + len(tail)
        )
        middle_mean = sum(p.pressure for p in middle) / len(middle)
        tapers.append(middle_mean - edge_mean)
    if not tapers:
        return None
    return sum(tapers) / len(tapers)


def _speed_stats(strokes: Sequence[Stroke]) -> tuple[float | None, float | None]:
    """按段长加权的速度均值与标准差（归一化坐标 / 秒）。"""
    speeds: list[float] = []
    weights: list[float] = []
    for stroke in strokes:
        for a, b in pairwise(stroke.points):
            dt_ms = b.t - a.t
            if dt_ms <= 0:
                continue
            length = math.hypot(b.x - a.x, b.y - a.y)
            if length <= _EPS:
                continue
            speeds.append(length / (dt_ms / 1000.0))
            weights.append(length)
    if not speeds:
        return None, None

    total_weight = sum(weights)
    mean = sum(s * w for s, w in zip(speeds, weights, strict=False)) / total_weight
    variance = (
        sum(w * (s - mean) ** 2 for s, w in zip(speeds, weights, strict=False)) / total_weight
    )
    return mean, math.sqrt(variance)


def _pause_ratio(strokes: Sequence[Stroke], start_times: Sequence[int] | None) -> float | None:
    """抬笔时间 / 总书写时间。需要每笔的绝对起始时刻。"""
    if start_times is None or len(start_times) != len(strokes) or not strokes:
        return None

    pen_down = sum(stroke.duration_ms for stroke in strokes)
    span = (start_times[-1] + strokes[-1].duration_ms) - start_times[0]
    if span <= 0:
        return None
    return max(0.0, min(1.0, (span - pen_down) / span))


def extract(
    stroke_set: StrokeSet,
    start_times: Sequence[int] | None = None,
    resample_step: float = DEFAULT_RESAMPLE_STEP,
) -> StyleFeatures:
    """提取一个字的风格特征。

    ``start_times`` 为每笔相对该字起笔时刻的毫秒数（采集格式中的 ``start_t``）。
    缺失时 ``pause_ratio`` 为 ``None``。
    """
    if not stroke_set.strokes:
        raise ValueError("空字形无法提取风格特征")

    resampled = [resample(s, resample_step) for s in stroke_set.strokes]

    box = stroke_set.bbox()
    assert box is not None  # strokes 非空，bbox 必然存在
    min_x, min_y, max_x, max_y = box
    width = max_x - min_x
    height = max_y - min_y
    diagonal = math.hypot(width, height)

    lengths = [s.length for s in resampled]
    total_length = sum(lengths)
    chords = sum(
        math.hypot(s.points[-1].x - s.points[0].x, s.points[-1].y - s.points[0].y)
        for s in resampled
    )

    turns: list[float] = []
    for stroke in resampled:
        turns.extend(_turn_angles_deg(stroke))

    length_mean = total_length / len(resampled)
    length_variance = sum((length - length_mean) ** 2 for length in lengths) / len(lengths)
    length_cv = math.sqrt(length_variance) / length_mean if length_mean > _EPS else 0.0

    pressures = [p.pressure for s in stroke_set.strokes for p in s.points]
    pressure_mean = sum(pressures) / len(pressures)
    pressure_variance = sum((value - pressure_mean) ** 2 for value in pressures) / len(pressures)

    speed_mean, speed_std = _speed_stats(stroke_set.strokes)
    corner_deg, corner_rate = _corner_stats(turns, total_length)

    return StyleFeatures(
        stroke_count=len(stroke_set.strokes),
        aspect_ratio=(width / height) if height > _EPS else 0.0,
        slant_deg=_slant_deg(resampled),
        ink_density=(total_length / diagonal) if diagonal > _EPS else 0.0,
        straightness=(chords / total_length) if total_length > _EPS else 0.0,
        turn_mean_deg=(sum(turns) / len(turns)) if turns else 0.0,
        corner_deg=corner_deg,
        corner_rate=corner_rate,
        stroke_length_cv=length_cv,
        pressure_mean=pressure_mean,
        pressure_std=math.sqrt(pressure_variance),
        pressure_taper=_pressure_taper(stroke_set.strokes),
        speed_mean=speed_mean,
        speed_std=speed_std,
        pause_ratio=_pause_ratio(stroke_set.strokes, start_times),
    )


def mean_features(items: Iterable[StyleFeatures]) -> StyleFeatures:
    """对多个样本求平均，得到书写者的整体风格向量。

    可用性取交集：只要有一个样本缺压感，结果就不给压感特征，
    避免用部分样本的均值冒充整体风格。
    """
    collected = list(items)
    if not collected:
        raise ValueError("至少需要一个样本")

    names = collected[0].numeric_fields
    averaged: dict[str, float | None] = {}
    for name in names:
        values = [getattr(item, name) for item in collected]
        if any(value is None for value in values):
            averaged[name] = None
        else:
            averaged[name] = sum(values) / len(values)  # type: ignore[arg-type]

    return StyleFeatures(
        stroke_count=round(sum(item.stroke_count for item in collected) / len(collected)),
        **averaged,  # type: ignore[arg-type]
    )


def distance(a: StyleFeatures, b: StyleFeatures) -> float:
    """两个风格向量之间的归一化欧氏距离。

    只比较双方都可用（非 ``None``）的特征，避免把「无压感」当成「压力为 0」。
    返回值 0 表示完全一致，约 1.0 表示各特征平均相差一个典型书写者差异。
    """
    total = 0.0
    counted = 0
    for name in a.numeric_fields:
        va = getattr(a, name)
        vb = getattr(b, name)
        if va is None or vb is None:
            continue
        total += ((va - vb) / _SCALE[name]) ** 2
        counted += 1
    return math.sqrt(total / counted) if counted else 0.0
