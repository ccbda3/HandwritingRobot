"""任意图形的轨迹生成 —— 表格 / 思维导图 / 矢量导入。

## 为什么需要这一层

项目管线从 ``Stroke`` 层面就是**图形无关**的：``render``（文字）、``gcode``、
``preview`` 都只认 ``StrokeSet``（归一化 0~1、y 向下）。所以「一个汉字」和
「一张表格」「一段导入的 SVG 路径」在下游看来没有区别，都能直接送进机器写。

这让用户那句「不只是要字，任何奇形怪状的都可以写」在产品层面成立，有三种入口：

- **手绘**：采集器「图形」模式自由画（见 ``capture/index.html``），导出为
  ``handwriting-capture/2.0`` 的 ``contentType:"shape"``；
- **规整生成**：本模块直接生成表格、思维导图这类结构化图形；
- **完全自定义**：把任意矢量图（SVG）导入，按中心线描红。

## 坐标系约定

与全项目一致：原点在方框左上角，x 向右、y **向下**，取值 0~1。SVG 本身也是
y 向下，因此导入时无需翻转；导入后统一 ``fit_to_box`` 归一化到 0~1。
"""

from __future__ import annotations

import math
import re
import xml.etree.ElementTree as ET
from collections.abc import Sequence
from dataclasses import dataclass
from typing import Any

from handwriting.render import render_text
from handwriting.stroke import Point, Stroke, StrokeSet

# 采样点之间的浮点噪声下限，避免长度接近 0 的线段产生无意义方向
_EPS = 1e-9


# ---------------------------------------------------------------------------
# 几何基元
# ---------------------------------------------------------------------------
def _poly(points: Sequence[tuple[float, float]], pressure: float = 0.5) -> Stroke:
    """把 ``[(x, y), ...]`` 转成一笔。坐标必须已在 0~1 范围内。"""
    return Stroke([Point(x, y, pressure, 0) for x, y in points])


def line_segment(x1: float, y1: float, x2: float, y2: float) -> StrokeSet:
    """一条直线（一笔）。"""
    return StrokeSet([_poly([(x1, y1), (x2, y2)])], "")


def rect_outline(x: float, y: float, w: float, h: float) -> StrokeSet:
    """矩形边框。**四边各为一笔**（机器人描边框时会抬笔，更真实）。"""
    return StrokeSet(
        [
            _poly([(x, y), (x, y + h)]),
            _poly([(x, y + h), (x + w, y + h)]),
            _poly([(x + w, y + h), (x + w, y)]),
            _poly([(x + w, y), (x, y)]),
        ],
        "",
    )


def _ellipse_points(cx: float, cy: float, rx: float, ry: float, segments: int = 64) -> list[tuple[float, float]]:
    pts: list[tuple[float, float]] = []
    for i in range(segments + 1):
        a = 2.0 * math.pi * i / segments
        pts.append((cx + rx * math.cos(a), cy + ry * math.sin(a)))
    return pts


def circle(cx: float, cy: float, r: float, segments: int = 64) -> StrokeSet:
    """圆（一笔，闭合）。"""
    return StrokeSet([_poly(_ellipse_points(cx, cy, r, r, segments))], "")


def ellipse(cx: float, cy: float, rx: float, ry: float, segments: int = 64) -> StrokeSet:
    """椭圆（一笔，闭合）。"""
    return StrokeSet([_poly(_ellipse_points(cx, cy, rx, ry, segments))], "")


def arrow(
    x1: float, y1: float, x2: float, y2: float, head_len: float = 0.08, head_angle_deg: float = 26.0
) -> StrokeSet:
    """带箭头的线段：主干一笔，箭头两笔。"""
    shaft = _poly([(x1, y1), (x2, y2)])
    ang = math.atan2(y2 - y1, x2 - x1)
    ha = math.radians(head_angle_deg)
    h1 = (x2 - head_len * math.cos(ang - ha), y2 - head_len * math.sin(ang - ha))
    h2 = (x2 - head_len * math.cos(ang + ha), y2 - head_len * math.sin(ang + ha))
    return StrokeSet([shaft, _poly([h1, (x2, y2)]), _poly([h2, (x2, y2)])], "")


def polyline(points: Sequence[tuple[float, float]]) -> StrokeSet:
    """折线（一笔）。"""
    return StrokeSet([_poly(list(points))], "")


# ---------------------------------------------------------------------------
# 文本落入方框（供表格 / 思维导图单元格复用）
# ---------------------------------------------------------------------------
def _text_in_box(
    text: str, box: tuple[float, float, float, float], source: Any
) -> StrokeSet:
    """把 ``text`` 渲染后放进 ``(x, y, w, h)`` 方框，左上对齐、等比缩放。

    每个字符各自归一化成边长 ``glyph_size`` 的正方形再左到右排布；多行用换行符。
    """
    result = render_text(text, source=source)
    if result.is_empty:
        return StrokeSet([], "")

    bx, by, bw, bh = box
    pad = 0.12
    avail_w = bw * (1.0 - 2.0 * pad)
    avail_h = bh * (1.0 - 2.0 * pad)
    lines = result.rows
    nlines = max(1, len(lines))
    line_h = avail_h / nlines
    max_chars = max((len(row) for row in lines), default=1) or 1
    glyph_size = min(avail_w / max_chars, line_h * 0.9)
    if glyph_size <= _EPS:
        return StrokeSet([], "")

    strokes: list[Stroke] = []
    for li, row in enumerate(lines):
        top = by + pad * bh + li * line_h + (line_h - glyph_size) / 2.0
        cursor = bx + pad * bw
        for glyph in row:
            fitted = glyph.fit_to_box(glyph_size, padding=0.0)
            for st in fitted.strokes:
                strokes.append(st.translated(cursor, top))
            cursor += glyph_size
    return StrokeSet(strokes, "")


# ---------------------------------------------------------------------------
# 结构化内容生成
# ---------------------------------------------------------------------------
def compose_table(
    rows: int,
    cols: int,
    texts: Sequence[Sequence[str]] | None = None,
    source: Any = None,
    padding: float = 0.05,
) -> StrokeSet:
    """生成一张表格的 ``StrokeSet``（归一化 0~1）。

    - 外边框 + 内部行列分隔线，每根线独立一笔；
    - ``texts[r][c]`` 提供单元格文字时，用 ``source`` 渲染后落入对应单元格
      （``source`` 为 ``None`` 时只画空表，离线也能用）。

    返回整张表作为一个 ``StrokeSet``，下游当作「一个图形」摆进一个单元格。
    """
    if rows < 1 or cols < 1:
        raise ValueError(f"表格行列数必须 ≥ 1，收到 {rows}×{cols}")
    if not 0.0 < padding < 0.45:
        raise ValueError(f"padding 必须落在 (0, 0.45)，收到 {padding}")

    x0, y0 = padding, padding
    w = (1.0 - 2.0 * padding)
    h = (1.0 - 2.0 * padding)
    cw, ch = w / cols, h / rows

    strokes: list[Stroke] = list(rect_outline(x0, y0, w, h).strokes)
    for c in range(1, cols):
        x = x0 + c * cw
        strokes.append(_poly([(x, y0), (x, y0 + h)]))
    for r in range(1, rows):
        y = y0 + r * ch
        strokes.append(_poly([(x0, y), (x0 + w, y)]))

    if texts and source is not None:
        for r in range(rows):
            for c in range(cols):
                cell_text = ""
                if r < len(texts) and c < len(texts[r]):
                    cell_text = texts[r][c]
                if not cell_text:
                    continue
                cell_box = (x0 + c * cw, y0 + r * ch, cw, ch)
                strokes += _text_in_box(cell_text, cell_box, source).strokes

    return StrokeSet(strokes, "")


@dataclass(slots=True)
class MindNode:
    """思维导图的一个节点（中心坐标与尺寸均在归一化 0~1 空间）。"""

    id: str
    x: float
    y: float
    w: float
    h: float
    label: str = ""


@dataclass(slots=True)
class MindEdge:
    """一条连接（按节点 ``id`` 引用）。"""

    a: str
    b: str


def _box_edge_point(node: MindNode, tx: float, ty: float) -> tuple[float, float]:
    """从节点中心指向 ``(tx, ty)`` 的射线与节点矩形边框的交点。"""
    hw, hh = node.w / 2.0, node.h / 2.0
    dx, dy = tx - node.x, ty - node.y
    if dx == 0 and dy == 0:
        return (node.x, node.y)
    scale_x = hw / abs(dx) if dx != 0 else math.inf
    scale_y = hh / abs(dy) if dy != 0 else math.inf
    s = min(scale_x, scale_y)
    return (node.x + dx * s, node.y + dy * s)


def compose_mindmap(
    nodes: Sequence[MindNode],
    edges: Sequence[MindEdge],
    source: Any = None,
    padding: float = 0.05,
) -> StrokeSet:
    """生成一张思维导图的 ``StrokeSet``（归一化 0~1）。

    节点画成方框，连线从边框连到边框，``label`` 用 ``source`` 渲染后落入节点框内。
    """
    if not nodes:
        return StrokeSet([], "")
    by_id = {n.id: n for n in nodes}

    strokes: list[Stroke] = []
    for n in nodes:
        strokes += rect_outline(n.x - n.w / 2.0, n.y - n.h / 2.0, n.w, n.h).strokes

    for e in edges:
        a, b = by_id.get(e.a), by_id.get(e.b)
        if a is None or b is None:
            continue
        p1 = _box_edge_point(a, b.x, b.y)
        p2 = _box_edge_point(b, a.x, a.y)
        strokes.append(_poly([p1, p2]))

    if source is not None:
        for n in nodes:
            if n.label:
                box = (n.x - n.w / 2.0, n.y - n.h / 2.0, n.w, n.h)
                strokes += _text_in_box(n.label, box, source).strokes

    return StrokeSet(strokes, "")


# ---------------------------------------------------------------------------
# SVG 矢量导入（通用适配器）
# ---------------------------------------------------------------------------
_PATH_CMD = re.compile(r"([MmLlHhVvCcSsQqTtAaZz])")
_NUM = re.compile(r"-?\d*\.?\d+(?:[eE][-+]?\d+)?")


def _localname(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def _parse_points(s: str | None) -> list[tuple[float, float]]:
    if not s:
        return []
    vals = [float(x) for x in _NUM.findall(s)]
    return [(vals[i], vals[i + 1]) for i in range(0, len(vals) - 1, 2)]


def _bezier(
    x0: float, y0: float, p1: tuple[float, float], p2: tuple[float, float], p3: tuple[float, float], n: int = 20
) -> list[tuple[float, float]]:
    out: list[tuple[float, float]] = []
    for i in range(1, n + 1):
        t = i / n
        mt = 1.0 - t
        x = mt**3 * x0 + 3 * mt**2 * t * p1[0] + 3 * mt * t**2 * p2[0] + t**3 * p3[0]
        y = mt**3 * y0 + 3 * mt**2 * t * p1[1] + 3 * mt * t**2 * p2[1] + t**3 * p3[1]
        out.append((x, y))
    return out


def _quad(
    x0: float, y0: float, c: tuple[float, float], p1: tuple[float, float], n: int = 12
) -> list[tuple[float, float]]:
    out: list[tuple[float, float]] = []
    for i in range(1, n + 1):
        t = i / n
        mt = 1.0 - t
        x = mt**2 * x0 + 2 * mt * t * c[0] + t**2 * p1[0]
        y = mt**2 * y0 + 2 * mt * t * c[1] + t**2 * p1[1]
        out.append((x, y))
    return out


def _arc_to_polyline(
    x1: float,
    y1: float,
    rx: float,
    ry: float,
    phi_deg: float,
    large_arc: bool,
    sweep: bool,
    x2: float,
    y2: float,
    step_deg: float = 2.0,
) -> list[tuple[float, float]]:
    """把椭圆弧（SVG A 指令）按角度采样成折线，避免贝塞尔控制点推导错误。"""
    phi = math.radians(phi_deg)
    rx, ry = abs(rx), abs(ry)
    dx, dy = (x1 - x2) / 2.0, (y1 - y2) / 2.0
    x1p = math.cos(phi) * dx + math.sin(phi) * dy
    y1p = -math.sin(phi) * dx + math.cos(phi) * dy

    lam = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry)
    if lam > 1.0:
        scale = math.sqrt(lam)
        rx, ry = rx * scale, ry * scale

    num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p
    den = rx * rx * y1p * y1p + ry * ry * x1p * x1p
    coef = math.sqrt(max(0.0, num / den)) if den != 0 else 0.0
    if large_arc == sweep:
        coef = -coef
    cxp = coef * (rx * y1p / ry)
    cyp = coef * (-ry * x1p / rx)
    cx = math.cos(phi) * cxp - math.sin(phi) * cyp + (x1 + x2) / 2.0
    cy = math.sin(phi) * cxp + math.cos(phi) * cyp + (y1 + y2) / 2.0

    theta1 = math.atan2((y1p - cyp) / ry, (x1p - cxp) / rx)
    theta2 = math.atan2((-y1p - cyp) / ry, (-x1p - cxp) / rx)
    dtheta = theta2 - theta1
    if not sweep and dtheta > 0:
        dtheta -= 2.0 * math.pi
    if sweep and dtheta < 0:
        dtheta += 2.0 * math.pi

    n = max(2, int(abs(dtheta) / math.radians(step_deg)) + 1)
    pts: list[tuple[float, float]] = []
    for i in range(n + 1):
        th = theta1 + dtheta * (i / n)
        ex, ey = rx * math.cos(th), ry * math.sin(th)
        px = math.cos(phi) * ex - math.sin(phi) * ey + cx
        py = math.sin(phi) * ex + math.cos(phi) * ey + cy
        pts.append((px, py))
    return pts


def _abs_point(x: float, y: float, cx: float, cy: float, rel: bool) -> tuple[float, float]:
    return (x + (cx if rel else 0.0), y + (cy if rel else 0.0))


def _parse_path(d: str) -> list[list[tuple[float, float]]]:
    """解析 SVG path 的 ``d`` 属性，返回若干条折线。"""
    commands = list(_PATH_CMD.finditer(d))
    polylines: list[list[tuple[float, float]]] = []
    cx = cy = 0.0
    sx = sy = 0.0
    subpath: list[tuple[float, float]] = []
    prev_ctrl: tuple[float, float] | None = None
    prev_cmd: str | None = None

    def flush() -> None:
        if len(subpath) >= 1:
            polylines.append(subpath)

    for idx, m in enumerate(commands):
        cmd = m.group(1)
        end = commands[idx + 1].start() if idx + 1 < len(commands) else len(d)
        nums = [float(x) for x in _NUM.findall(d[m.end():end])]
        upper = cmd.upper()
        rel = cmd.islower()

        if upper == "M":
            for i in range(0, len(nums) - 1, 2):
                px, py = _abs_point(nums[i], nums[i + 1], cx, cy, rel)
                if i == 0:
                    flush()
                    subpath = [(px, py)]
                    sx, sy = px, py
                else:
                    subpath.append((px, py))
                cx, cy = px, py
            prev_ctrl = None
        elif upper == "L":
            for i in range(0, len(nums) - 1, 2):
                px, py = _abs_point(nums[i], nums[i + 1], cx, cy, rel)
                subpath.append((px, py))
                cx, cy = px, py
            prev_ctrl = None
        elif upper == "H":
            for x in nums:
                px = x + (cx if rel else 0.0)
                subpath.append((px, cy))
                cx = px
            prev_ctrl = None
        elif upper == "V":
            for y in nums:
                py = y + (cy if rel else 0.0)
                subpath.append((cx, py))
                cy = py
            prev_ctrl = None
        elif upper == "C":
            for i in range(0, len(nums) - 5, 6):
                p1 = _abs_point(nums[i], nums[i + 1], cx, cy, rel)
                p2 = _abs_point(nums[i + 2], nums[i + 3], cx, cy, rel)
                p = _abs_point(nums[i + 4], nums[i + 5], cx, cy, rel)
                subpath += _bezier(cx, cy, p1, p2, p)
                cx, cy = p
                prev_ctrl, prev_cmd = p2, "C"
        elif upper == "S":
            for i in range(0, len(nums) - 3, 4):
                p2 = _abs_point(nums[i], nums[i + 1], cx, cy, rel)
                p = _abs_point(nums[i + 2], nums[i + 3], cx, cy, rel)
                c1 = (
                    (2 * cx - prev_ctrl[0], 2 * cy - prev_ctrl[1])
                    if prev_cmd in ("C", "S") and prev_ctrl
                    else (cx, cy)
                )
                subpath += _bezier(cx, cy, c1, p2, p)
                cx, cy = p
                prev_ctrl, prev_cmd = p2, "S"
        elif upper == "Q":
            for i in range(0, len(nums) - 3, 4):
                c = _abs_point(nums[i], nums[i + 1], cx, cy, rel)
                p = _abs_point(nums[i + 2], nums[i + 3], cx, cy, rel)
                subpath += _quad(cx, cy, c, p)
                cx, cy = p
                prev_ctrl, prev_cmd = c, "Q"
        elif upper == "T":
            for i in range(0, len(nums) - 1, 2):
                p = _abs_point(nums[i], nums[i + 1], cx, cy, rel)
                c = (
                    (2 * cx - prev_ctrl[0], 2 * cy - prev_ctrl[1])
                    if prev_cmd in ("Q", "T") and prev_ctrl
                    else (cx, cy)
                )
                subpath += _quad(cx, cy, c, p)
                cx, cy = p
                prev_ctrl, prev_cmd = c, "T"
        elif upper == "A":
            for i in range(0, len(nums) - 6, 7):
                rx, ry, rot, la, sw, x, y = nums[i : i + 7]
                p = _abs_point(x, y, cx, cy, rel)
                arc = _arc_to_polyline(cx, cy, rx, ry, rot, bool(la), bool(sw), p[0], p[1])
                if arc:
                    subpath += arc[1:]  # arc[0] 即当前点，去重
                cx, cy = p
            prev_ctrl = None
        elif upper == "Z":
            if subpath:
                subpath.append((sx, sy))
            prev_ctrl = None

    flush()
    return polylines


def svg_to_stroke_set(svg_text: str, padding: float = 0.02) -> StrokeSet:
    """把一段 SVG 解析成 ``StrokeSet``（归一化 0~1，y 向下）。

    支持 ``path / line / polyline / polygon / rect / circle / ellipse``。
    路径中的椭圆弧（A）按角度采样成折线。``<text>`` 不参与解析 —— 文字应作为
    独立的「手写层」用字形管线生成，而非描边字体轮廓。

    所有图形统一 ``fit_to_box`` 归一化到 0~1 方框（保留纵横比）。
    """
    try:
        root = ET.fromstring(svg_text)
    except ET.ParseError as exc:
        raise ValueError(f"SVG 解析失败：{exc}") from exc

    polylines: list[list[tuple[float, float]]] = []
    for el in root.iter():
        tag = _localname(el.tag)
        if tag == "path":
            d = el.get("d")
            if d:
                polylines += _parse_path(d)
        elif tag == "line":
            polylines.append(
                [
                    (float(el.get("x1", "0")), float(el.get("y1", "0"))),
                    (float(el.get("x2", "0")), float(el.get("y2", "0"))),
                ]
            )
        elif tag == "polyline":
            pts = _parse_points(el.get("points"))
            if pts:
                polylines.append(pts)
        elif tag == "polygon":
            pts = _parse_points(el.get("points"))
            if pts:
                polylines.append([*pts, pts[0]])
        elif tag == "rect":
            x, y = float(el.get("x", "0")), float(el.get("y", "0"))
            w, h = float(el.get("width", "0")), float(el.get("height", "0"))
            if w > 0 and h > 0:
                polylines.append([(x, y), (x + w, y), (x + w, y + h), (x, y + h), (x, y)])
        elif tag == "circle":
            cx, cy, r = float(el.get("cx", "0")), float(el.get("cy", "0")), float(el.get("r", "0"))
            if r > 0:
                polylines.append(_ellipse_points(cx, cy, r, r))
        elif tag == "ellipse":
            cx = float(el.get("cx", "0"))
            cy = float(el.get("cy", "0"))
            rx, ry = float(el.get("rx", "0")), float(el.get("ry", "0"))
            if rx > 0 and ry > 0:
                polylines.append(_ellipse_points(cx, cy, rx, ry))

    if not polylines:
        return StrokeSet([], "")

    strokes = [
        Stroke([Point(x, y, 0.5, 0) for x, y in pl]) for pl in polylines if len(pl) >= 1
    ]
    return StrokeSet(strokes, "").fit_to_box(1.0, padding=padding)


__all__ = [
    "MindEdge",
    "MindNode",
    "arrow",
    "circle",
    "compose_mindmap",
    "compose_table",
    "ellipse",
    "line_segment",
    "polyline",
    "rect_outline",
    "svg_to_stroke_set",
]
