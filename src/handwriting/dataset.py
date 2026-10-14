"""读取采集器（capture/index.html）导出的笔迹数据集。"""

from __future__ import annotations

import json
from collections.abc import Iterator
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from handwriting.stroke import Stroke, StrokeSet, strokes_from_arrays

SUPPORTED_FORMATS = ("handwriting-capture/1.0",)


@dataclass(slots=True)
class CaptureMeta:
    """数据集元信息，描述采集环境 —— 训练前必须核对。"""

    device: str = "unknown"
    pressure_supported: bool = False
    sampling_hz: int | None = None
    char_count: int | None = None
    cell_px: int | None = None
    created_at: str | None = None
    coord_system: str | None = None
    raw: dict[str, Any] = field(default_factory=dict)

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> CaptureMeta:
        return cls(
            device=str(data.get("device", "unknown")),
            pressure_supported=bool(data.get("pressureSupported", False)),
            sampling_hz=data.get("samplingHz"),
            char_count=data.get("charCount"),
            cell_px=data.get("cellPx"),
            created_at=data.get("createdAt"),
            coord_system=data.get("coordSystem"),
            raw=dict(data),
        )


@dataclass(slots=True)
class CaptureSample:
    """一个字的一次书写记录。"""

    index: int
    char: str
    strokes: list[Stroke]
    rate: int = 0
    pressure: bool = False

    @property
    def stroke_count(self) -> int:
        return len(self.strokes)

    @property
    def point_count(self) -> int:
        return sum(len(s) for s in self.strokes)

    def to_stroke_set(self, fit: bool = False) -> StrokeSet:
        """转为 StrokeSet；``fit=True`` 时等比归一化到单位方框。"""
        result = StrokeSet(list(self.strokes), self.char)
        return result.fit_to_box(1.0) if fit else result


@dataclass(slots=True)
class CaptureData:
    """一份完整的采集数据集。"""

    meta: CaptureMeta
    samples: list[CaptureSample]
    source: Path | None = None

    @classmethod
    def load(cls, path: str | Path) -> CaptureData:
        path = Path(path)
        payload = json.loads(path.read_text(encoding="utf-8"))
        return cls.from_dict(payload, source=path)

    @classmethod
    def from_dict(cls, payload: dict[str, Any], source: Path | None = None) -> CaptureData:
        fmt = payload.get("format")
        if fmt not in SUPPORTED_FORMATS:
            raise ValueError(f"不支持的数据格式 {fmt!r}，期望 {SUPPORTED_FORMATS}")

        raw_samples = payload.get("samples")
        if not isinstance(raw_samples, list):
            raise ValueError("samples 字段缺失或不是列表")

        samples: list[CaptureSample] = []
        for i, item in enumerate(raw_samples):
            samples.append(
                CaptureSample(
                    index=int(item.get("index", i)),
                    char=str(item.get("char", "")),
                    strokes=strokes_from_arrays(item.get("strokes", [])),
                    rate=int(item.get("rate", 0) or 0),
                    pressure=bool(item.get("pressure", False)),
                )
            )
        return cls(
            meta=CaptureMeta.from_dict(payload.get("meta", {})), samples=samples, source=source
        )

    @property
    def char_count(self) -> int:
        return len(self.samples)

    @property
    def total_points(self) -> int:
        return sum(s.point_count for s in self.samples)

    @property
    def average_points_per_char(self) -> float:
        return self.total_points / self.char_count if self.char_count else 0.0

    def by_char(self, char: str) -> list[CaptureSample]:
        return [s for s in self.samples if s.char == char]

    def iter_stroke_sets(self, fit: bool = False) -> Iterator[StrokeSet]:
        for sample in self.samples:
            yield sample.to_stroke_set(fit=fit)

    def validate(self) -> list[str]:
        """做基本质量体检，返回问题清单（空列表表示通过）。"""
        problems: list[str] = []
        if not self.meta.pressure_supported:
            problems.append("数据集未包含压感信息，无法还原笔画粗细变化")
        if self.meta.sampling_hz is not None and self.meta.sampling_hz < 100:
            problems.append(
                f"采样率偏低（{self.meta.sampling_hz} Hz），快速连笔会丢细节，建议不低于 100 Hz"
            )
        for sample in self.samples:
            if not sample.char:
                problems.append(f"第 {sample.index} 个样本缺少字符标签")
            if sample.stroke_count == 0:
                problems.append(f"第 {sample.index} 个样本（{sample.char}）没有笔画")
        return problems
