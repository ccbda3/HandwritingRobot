"""手写笔迹轨迹处理与 G 代码生成。

本包只处理**联机笔迹**（带笔序、时序、抬落笔信息的笔尖坐标序列）。
图像 → 轨迹的转换属于独立的离线通道，见 docs/architecture.md。

模块划分：

``stroke``
    轨迹数据结构与几何变换
``curve``
    曲线插值与重采样（字形平滑与特征提取共用）
``glyph``
    从标准笔顺库加载笔画中心线
``layout``
    版面计算（横条本 / 自由版面）
``render``
    文本 → 归一化轨迹（倾斜、抖动、笔压、时序合成）
``preview``
    轨迹 → SVG 预览
``style``
    风格特征提取（主动选字与生成质量评估的共同基础）
``curriculum``
    字表元数据与覆盖度驱动的主动选字策略
``dataset``
    读取采集器导出的 JSON 数据集（单字，``handwriting-capture/1.0``）
``segment``
    整行笔迹的「字」分段（抬笔间隔 + 已知字数约束）
``line``
    读取整行采集数据集（``handwriting-capture/2.0``）
``shapes``
    任意图形内容层（表格 / 思维导图 / SVG 导入）
``sheets``
    把若干图形摆上版面并出 G 代码 / 预览
``gcode``
    GRBL G 代码生成
``cli``
    命令行入口
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
from handwriting.gcode import (
    PlotSettings,
    char_origin,
    glyphs_to_gcode_with_stats,
    page_size,
    strokes_to_gcode,
)
from handwriting.glyph import (
    EM_SIZE,
    Glyph,
    GlyphNotFoundError,
    HanziWriterSource,
    load_glyphs,
)
from handwriting.layout import (
    NOTEBOOK_HEIGHT_MM,
    NOTEBOOK_LINES,
    NOTEBOOK_WIDTH_MM,
    Cell,
    LayoutSettings,
    Mode,
    Page,
    compute,
    resolve_char_size,
    resolve_chars_per_line,
)
from handwriting.line import LineCapture, LineSample
from handwriting.render import RenderResult, RenderSettings, render_text
from handwriting.segment import DEFAULT_GAP_MS, segment_by_gap, segment_line, stroke_gaps
from handwriting.shapes import (
    MindEdge,
    MindNode,
    arrow,
    circle,
    compose_mindmap,
    compose_table,
    ellipse,
    line_segment,
    polyline,
    rect_outline,
    svg_to_stroke_set,
)
from handwriting.sheets import ShapeSheet, compose
from handwriting.stroke import Point, Stroke, StrokeSet
from handwriting.style import (
    LINE_CONNECT_MS,
    LineStyleFeatures,
    StyleFeatures,
    distance,
    extract,
    extract_line,
    mean_features,
    mean_line_features,
    resample,
)

__version__ = "0.5.0"

__all__ = [
    "CHARACTERS",
    "COLD_START_CHARS",
    "DEFAULT_GAP_MS",
    "EM_SIZE",
    "LINE_CONNECT_MS",
    "NOTEBOOK_HEIGHT_MM",
    "NOTEBOOK_LINES",
    "NOTEBOOK_WIDTH_MM",
    "CaptureData",
    "CaptureMeta",
    "CaptureSample",
    "Cell",
    "CharacterMeta",
    "Glyph",
    "GlyphNotFoundError",
    "HanziWriterSource",
    "LayoutSettings",
    "LineCapture",
    "LineSample",
    "LineStyleFeatures",
    "MindEdge",
    "MindNode",
    "Mode",
    "Page",
    "PlotSettings",
    "Point",
    "RenderResult",
    "RenderSettings",
    "ShapeSheet",
    "Stroke",
    "StrokeSet",
    "StyleFeatures",
    "__version__",
    "arrow",
    "char_origin",
    "circle",
    "compose",
    "compose_mindmap",
    "compose_table",
    "compute",
    "coverage_report",
    "distance",
    "ellipse",
    "extract",
    "extract_line",
    "glyphs_to_gcode_with_stats",
    "line_segment",
    "load_glyphs",
    "mean_features",
    "mean_line_features",
    "next_char",
    "order_by_coverage",
    "page_size",
    "polyline",
    "rect_outline",
    "render_text",
    "resample",
    "resolve_char_size",
    "resolve_chars_per_line",
    "segment_by_gap",
    "segment_line",
    "stroke_gaps",
    "strokes_to_gcode",
    "svg_to_stroke_set",
]
