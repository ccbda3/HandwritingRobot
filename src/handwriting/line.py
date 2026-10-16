"""整行（连续书写）笔迹的数据结构与格式读写。

与 :mod:`handwriting.dataset`（单字采集，``handwriting-capture/1.0``）并列：

- 单字样本归一化到「一个字」的方框 —— 干净，用于笔画骨架 / 单字形态；
- 整行样本归一化到「一整行」的方框，**保留字与字之间的相对位置** ——
  能表达位置飘逸、字距、行气、连笔潦草、字间节奏，这些是单字采不到的。

坐标约定与单字一致（x 向右、y 向下、pressure 0~1、t 为毫秒），
区别只在归一化基准是整个行方框，而不是单个字格。

``strokeStartT`` 在这里是**相对整行起笔**的毫秒数（单字格式里是相对该字起笔），
这样才能还原字与字之间的停顿。
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from handwriting.dataset import CaptureMeta
from handwriting.segment import DEFAULT_GAP_MS, segment_line
from handwriting.stroke import Stroke, StrokeSet, strokes_from_arrays
from handwriting.style import DEFAULT_RESAMPLE_STEP, LineStyleFeatures, extract_line

SUPPORTED_FORMATS = ("handwriting-capture/2.0",)


@dataclass(slots=True)
class LineSample:
    """一整行的连续书写记录。"""

    index: int
    text: str
    strokes: list[Stroke]
    stroke_start_t: list[int]
    char_ranges: list[tuple[int, int]] | None = None
    """每个字占用的笔画区间 ``[start, end)``，与 ``text`` 逐字对应。缺失时为 None。"""
    rate: int = 0
    pressure: bool = False

    @property
    def char_count(self) -> int:
        return len(self.text)

    @property
    def stroke_count(self) -> int:
        return len(self.strokes)

    @property
    def point_count(self) -> int:
        return sum(len(s) for s in self.strokes)

    def segment(self, gap_ms: float = DEFAULT_GAP_MS) -> list[tuple[int, int]]:
        """就地计算并写入 ``char_ranges``，返回逐字的笔画区间。

        只依赖抬笔间隔 + 已知字数，见 :func:`handwriting.segment.segment_line`。
        """
        self.char_ranges = segment_line(self.strokes, self.stroke_start_t, self.text, gap_ms)
        return self.char_ranges

    def char_start_times(self) -> list[int] | None:
        """每个字的起笔时刻（毫秒，相对整行起笔）。缺 ``char_ranges`` 时返回 None。"""
        if not self.char_ranges:
            return None
        return [self.stroke_start_t[start] for start, _ in self.char_ranges]

    def to_stroke_set(self) -> StrokeSet:
        """整行作为一个 ``StrokeSet``（笔画按书写顺序）。"""
        return StrokeSet(list(self.strokes), self.text)

    def split_chars(self, fit: bool = False) -> list[tuple[str, StrokeSet]]:
        """按 ``char_ranges`` 切成单字样本 —— 一份整行数据两用。

        ``fit=True`` 时每个字各自等比归一化到单位方框（与单字采集口径一致）；
        ``fit=False`` 时保留它在行内的原始位置与大小（可用于研究位置飘逸）。
        """
        if not self.char_ranges or len(self.char_ranges) != len(self.text):
            raise ValueError("缺少与文本逐字对应的 char_ranges，无法切分")

        result: list[tuple[str, StrokeSet]] = []
        for char, (start, end) in zip(self.text, self.char_ranges, strict=True):
            stroke_set = StrokeSet(self.strokes[start:end], char)
            result.append((char, stroke_set.fit_to_box(1.0) if fit else stroke_set))
        return result

    def style_features(self, resample_step: float = DEFAULT_RESAMPLE_STEP) -> LineStyleFeatures:
        """行级风格特征：字宽/字高抖动、字距、基线漂移、字间停顿、连笔率等。

        缺 ``char_ranges`` 时先自动分段（见 :meth:`segment`）。
        """
        if not self.char_ranges:
            self.segment()
        assert self.char_ranges is not None  # segment() 之后必然有
        return extract_line(self.strokes, self.char_ranges, self.stroke_start_t, resample_step)


@dataclass(slots=True)
class LineCapture:
    """一份完整的整行采集数据集。"""

    meta: CaptureMeta
    lines: list[LineSample]
    source: Path | None = None

    @classmethod
    def load(cls, path: str | Path) -> LineCapture:
        path = Path(path)
        payload = json.loads(path.read_text(encoding="utf-8"))
        return cls.from_dict(payload, source=path)

    @classmethod
    def from_dict(cls, payload: dict[str, Any], source: Path | None = None) -> LineCapture:
        fmt = payload.get("format")
        if fmt not in SUPPORTED_FORMATS:
            raise ValueError(f"不支持的数据格式 {fmt!r}，期望 {SUPPORTED_FORMATS}")

        raw_lines = payload.get("lines")
        if not isinstance(raw_lines, list):
            raise ValueError("lines 字段缺失或不是列表")

        lines: list[LineSample] = []
        for i, item in enumerate(raw_lines):
            raw_ranges = item.get("charRanges")
            lines.append(
                LineSample(
                    index=int(item.get("index", i)),
                    text=str(item.get("text", "")),
                    strokes=strokes_from_arrays(item.get("strokes", [])),
                    stroke_start_t=[int(v) for v in item.get("strokeStartT", [])],
                    char_ranges=([(int(a), int(b)) for a, b in raw_ranges] if raw_ranges else None),
                    rate=int(item.get("rate", 0) or 0),
                    pressure=bool(item.get("pressure", False)),
                )
            )
        return cls(meta=CaptureMeta.from_dict(payload.get("meta", {})), lines=lines, source=source)

    @property
    def line_count(self) -> int:
        return len(self.lines)

    @property
    def total_strokes(self) -> int:
        return sum(line.stroke_count for line in self.lines)

    @property
    def total_points(self) -> int:
        return sum(line.point_count for line in self.lines)

    @property
    def total_chars(self) -> int:
        return sum(line.char_count for line in self.lines)

    def validate(self) -> list[str]:
        """质量体检，返回问题清单（空列表表示通过）。"""
        problems: list[str] = []
        if not self.meta.pressure_supported:
            problems.append("数据集未包含压感信息，无法还原笔画粗细变化")
        if self.meta.sampling_hz is not None and self.meta.sampling_hz < 100:
            problems.append(
                f"采样率偏低（{self.meta.sampling_hz} Hz），快速连笔会丢细节，建议不低于 100 Hz"
            )

        for line in self.lines:
            if not line.text:
                problems.append(f"第 {line.index} 行缺少文本")
            if line.stroke_count == 0:
                problems.append(f"第 {line.index} 行没有笔画")
                continue
            if len(line.stroke_start_t) != line.stroke_count:
                problems.append(
                    f"第 {line.index} 行 strokeStartT 数量（{len(line.stroke_start_t)}）"
                    f"与笔画数（{line.stroke_count}）不一致"
                )
            if line.char_ranges is not None:
                problems.extend(_check_char_ranges(line))
        return problems


def _check_char_ranges(line: LineSample) -> list[str]:
    """校验 ``char_ranges`` 是否逐字覆盖全部笔画、且不重不漏。"""
    assert line.char_ranges is not None
    problems: list[str] = []
    if len(line.char_ranges) != len(line.text):
        problems.append(
            f"第 {line.index} 行 charRanges 数量（{len(line.char_ranges)}）"
            f"与文本字数（{len(line.text)}）不一致"
        )
        return problems

    expected = 0
    for start, end in line.char_ranges:
        if start != expected or end <= start:
            problems.append(f"第 {line.index} 行 charRanges 不连续或顺序错误")
            return problems
        expected = end
    if expected != line.stroke_count:
        problems.append(f"第 {line.index} 行 charRanges 未覆盖全部笔画")
    return problems
