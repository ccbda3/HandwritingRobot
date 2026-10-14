"""手写笔迹轨迹处理与 G 代码生成。

本包只处理**联机笔迹**（带笔序、时序、抬落笔信息的笔尖坐标序列），
不处理图像。图像 → 轨迹的转换属于独立的离线通道，见 docs/architecture.md。

模块划分：
    stroke      轨迹数据结构与几何变换
    style       风格特征提取（主动选字与生成质量评估的共同基础）
    curriculum  字表元数据与覆盖度驱动的主动选字策略
    dataset     读取采集器导出的 JSON 数据集
    gcode       GRBL G 代码生成
"""

from handwriting.curriculum import (
    CHARACTERS,
    COLD_START_CHARS,
    CharacterMeta,
    coverage_report,
    next_char,
    order_by_coverage,
)
from handwriting.dataset import CaptureData, CaptureMeta, CaptureSample
from handwriting.gcode import PlotSettings, strokes_to_gcode
from handwriting.stroke import Point, Stroke, StrokeSet
from handwriting.style import StyleFeatures, distance, extract, mean_features, resample

__version__ = "0.2.0"

__all__ = [
    "CHARACTERS",
    "COLD_START_CHARS",
    "CaptureData",
    "CaptureMeta",
    "CaptureSample",
    "CharacterMeta",
    "PlotSettings",
    "Point",
    "Stroke",
    "StrokeSet",
    "StyleFeatures",
    "__version__",
    "coverage_report",
    "distance",
    "extract",
    "mean_features",
    "next_char",
    "order_by_coverage",
    "resample",
    "strokes_to_gcode",
]
