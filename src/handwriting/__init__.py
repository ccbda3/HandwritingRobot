"""手写笔迹轨迹处理与 G 代码生成。

本包只处理**联机笔迹**（带笔序、时序、抬落笔信息的笔尖坐标序列），
不处理图像。图像 → 轨迹的转换属于独立的离线通道，见 docs/architecture.md。

模块划分：
    stroke   轨迹数据结构与几何变换
    dataset  读取采集器导出的 JSON 数据集
    gcode    GRBL G 代码生成
"""

from handwriting.dataset import CaptureData, CaptureMeta, CaptureSample
from handwriting.gcode import PlotSettings, strokes_to_gcode
from handwriting.stroke import Point, Stroke, StrokeSet

__version__ = "0.1.0"

__all__ = [
    "CaptureData",
    "CaptureMeta",
    "CaptureSample",
    "PlotSettings",
    "Point",
    "Stroke",
    "StrokeSet",
    "__version__",
    "strokes_to_gcode",
]
