"""文本 → 归一化轨迹。

## 职责边界

本模块只产出**归一化坐标**（0~1）的 ``StrokeSet``。毫米级版面、进给速度、
G 代码由 :mod:`handwriting.gcode` 负责。这样字形渲染与机器参数解耦：
调字距不影响 G 代码逻辑，调进给速度也不影响字形。

## 风格从哪来

当前阶段没有风格模型，输出的字形来自标准笔顺骨架，只有下面几个**可调参数**
在模拟书写差异：倾斜度、抖动强度、笔压提按、书写速度。

必须说清楚：**这些参数不等于笔迹风格**。真正的个人风格（笔画粗细分布、
转折习惯、连笔倾向）只能由风格模型从用户样本中学习。这里做的是让骨架
能跑通硬件链路，并提供一个可对照的基线。
"""

from __future__ import annotations

import math
import random
from dataclasses import dataclass, field, replace
from typing import Protocol

from handwriting.glyph import (
    DEFAULT_RESAMPLE_STEP,
    DEFAULT_SAMPLES_PER_SEGMENT,
    Glyph,
    GlyphSource,
    HanziWriterSource,
)
from handwriting.stroke import Point, Stroke, StrokeSet

_EPS = 1e-9


@dataclass(slots=True)
class RenderSettings:
    """字形渲染参数。所有几何量都是归一化单位（字格边长 = 1）。"""

    slant_deg: float = 0.0
    """字形倾斜角，**右倾为正**。楷书通常 0~5°，行书可达 10° 以上。"""

    jitter: float = 0.0
    """手写抖动强度（高斯标准差）。建议 ≤ 0.005，0 为纯骨架。"""

    jitter_seed: int = 20261015
    """抖动随机种子。固定种子保证同一输入产出同一结果，便于复现问题。"""

    resample_step: float = DEFAULT_RESAMPLE_STEP
    samples_per_segment: int = DEFAULT_SAMPLES_PER_SEGMENT
    smooth: bool = True

    pressure_base: float = 0.45
    pressure_amplitude: float = 0.30
    """提按幅度：笔画两端相对中段重多少。0 为等压。"""

    dot_length_threshold: float = 0.12
    """短于该长度的笔画按「点」处理，用单峰压力曲线。"""

    writing_speed: float = 2.5
    """合成时序用的书写速度，归一化单位 / 秒。"""


@dataclass(slots=True)
class RenderResult:
    """渲染结果。``rows`` 中每个元素是一行，行内每个元素是一个字。"""

    rows: list[list[StrokeSet]]
    missing: list[str]
    """字形库中缺失、因而被跳过的字符（去重，保持出现顺序）。"""

    failures: dict[str, str] = field(default_factory=dict)
    """``{字符: 失败原因}``。用于区分「字形库里确实没有」与「网络抖动」。"""

    @property
    def char_count(self) -> int:
        return sum(len(row) for row in self.rows)

    @property
    def stroke_count(self) -> int:
        return sum(glyph.stroke_count for row in self.rows for glyph in row)

    @property
    def point_count(self) -> int:
        return sum(glyph.point_count for row in self.rows for glyph in row)

    @property
    def is_empty(self) -> bool:
        return self.char_count == 0


def apply_slant(stroke_set: StrokeSet, slant_deg: float) -> StrokeSet:
    """绕字格中心错切，模拟字形倾斜。右倾为正。

    绕中心而不是绕原点：绕原点错切会把字形整体推出字格，
    而且上半与下半的偏移方向相反、幅度还不对称。
    """
    if abs(slant_deg) < _EPS:
        return stroke_set

    factor = math.tan(math.radians(slant_deg))
    strokes = [
        Stroke([replace(point, x=point.x - factor * (point.y - 0.5)) for point in stroke.points])
        for stroke in stroke_set.strokes
    ]
    return StrokeSet(strokes, stroke_set.char)


def apply_jitter(stroke_set: StrokeSet, amplitude: float, rng: random.Random) -> StrokeSet:
    """给每个采样点加高斯扰动 —— 「手写自然抖动强度」参数。

    注意边界：抖动**不等于风格**。真正的笔迹风格来自风格模型，
    这里的抖动只是让骨架不至于机械般完美。幅度必须远小于真实书写的
    不规则性，否则会盖过字形本身的结构信息。
    """
    if amplitude <= 0.0:
        return stroke_set

    strokes = [
        Stroke(
            [
                replace(
                    point,
                    x=point.x + rng.gauss(0.0, amplitude),
                    y=point.y + rng.gauss(0.0, amplitude),
                )
                for point in stroke.points
            ]
        )
        for stroke in stroke_set.strokes
    ]
    return StrokeSet(strokes, stroke_set.char)


def apply_pressure(stroke_set: StrokeSet, settings: RenderSettings) -> StrokeSet:
    """合成笔压曲线。

    字形数据没有压感。这里按楷书的提按规律合成：

    - 长笔画（横、竖）—— **两头重、中段轻**，即起笔顿、行笔提、收笔顿
    - 短笔画（点）—— 单峰，中段最重

    这是**占位实现**：真正的提按取决于笔画类型（撇要收细、捺要放粗），
    而字形数据只给中心线、不给笔画类型。等风格模型接入后，压感应由模型
    直接生成，本函数退化为兜底。
    """
    strokes = []
    for stroke in stroke_set.strokes:
        count = len(stroke.points)
        is_dot = stroke.length < settings.dot_length_threshold
        points = []
        for index, point in enumerate(stroke.points):
            t = index / (count - 1) if count > 1 else 0.5
            shape = math.sin(math.pi * t) if is_dot else 1.0 - math.sin(math.pi * t)
            value = settings.pressure_base + settings.pressure_amplitude * shape
            points.append(replace(point, pressure=min(1.0, max(0.0, value))))
        strokes.append(Stroke(points))
    return StrokeSet(strokes, stroke_set.char)


def apply_timing(stroke_set: StrokeSet, settings: RenderSettings) -> StrokeSet:
    """按假定书写速度合成时序。

    字形数据没有时间信息。合成时序不是为了「让轨迹更真实」——
    真实时序必须来自风格模型 —— 而是为了让下游的 ``speed_mean``
    与 G 代码的进给速度规划有依据可循。

    同一笔内按累积弧长分配时间。笔与笔之间的间隔由 G 代码的抬笔移动承担，
    不在这里表达（采集格式里的 ``strokeStartT`` 才有笔间间隔的语义）。
    """
    strokes = []
    for stroke in stroke_set.strokes:
        points: list[Point] = []
        elapsed_ms = 0.0
        previous: Point | None = None
        for point in stroke.points:
            if previous is not None:
                step = math.hypot(point.x - previous.x, point.y - previous.y)
                elapsed_ms += step / settings.writing_speed * 1000.0
            points.append(replace(point, t=round(elapsed_ms)))
            previous = point
        strokes.append(Stroke(points))
    return StrokeSet(strokes, stroke_set.char)


def render_glyph(glyph: Glyph, settings: RenderSettings, rng: random.Random) -> StrokeSet:
    """把一个字形骨架渲染成可直接送进 G 代码生成器的轨迹。"""
    stroke_set = glyph.to_stroke_set(
        step=settings.resample_step,
        samples_per_segment=settings.samples_per_segment,
        smooth=settings.smooth,
    )
    # 顺序有讲究：抖动在倾斜之后（抖动不应被倾斜拉偏），
    # 压感与时序在最后（与几何无关，且时序依赖最终几何长度）
    stroke_set = apply_slant(stroke_set, settings.slant_deg)
    stroke_set = apply_jitter(stroke_set, settings.jitter, rng)
    stroke_set = apply_pressure(stroke_set, settings)
    return apply_timing(stroke_set, settings)


class TextSource(Protocol):
    """仅用于类型标注：字形数据源。"""

    def load(self, char: str) -> Glyph:  # pragma: no cover - 协议声明
        ...


def render_text(
    text: str,
    source: GlyphSource | None = None,
    settings: RenderSettings | None = None,
    chars_per_line: int | None = None,
) -> RenderResult:
    """把文本渲染成按行分组的字形轨迹。

    - 换行符 ``\\n`` 强制断行
    - 空白字符（空格、制表符）跳过
    - ``chars_per_line`` 为 ``None`` 时不自动折行
    - 字形库中缺失的字符被跳过并记录在 :attr:`RenderResult.missing`，
      不影响其余字符 —— 生僻字或标点缺失不该让整段文本渲染不出来
    """
    if chars_per_line is not None and chars_per_line < 1:
        raise ValueError(f"chars_per_line 必须 ≥ 1 或为 None，收到 {chars_per_line}")

    config = settings if settings is not None else RenderSettings()
    active = source if source is not None else HanziWriterSource()
    rng = random.Random(config.jitter_seed)

    rows: list[list[StrokeSet]] = []
    current: list[StrokeSet] = []
    missing: list[str] = []
    failures: dict[str, str] = {}

    for char in text:
        if char == "\n":
            if current:
                rows.append(current)
                current = []
            continue
        if char.isspace():
            continue
        if chars_per_line is not None and len(current) >= chars_per_line:
            rows.append(current)
            current = []

        try:
            glyph = active.load(char)
        except (LookupError, OSError, ValueError) as exc:
            if char not in failures:
                failures[char] = str(exc)
                missing.append(char)
            continue
        current.append(render_glyph(glyph, config, rng))

    if current:
        rows.append(current)
    return RenderResult(rows=rows, missing=missing, failures=failures)
