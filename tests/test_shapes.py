"""任意图形（表格 / 思维导图 / SVG 导入）的内容层测试。"""

from __future__ import annotations

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
from handwriting.stroke import StrokeSet


def test_primitives_return_valid_stroke_sets() -> None:
    for ss in (
        line_segment(0.1, 0.1, 0.9, 0.9),
        rect_outline(0.1, 0.1, 0.8, 0.8),
        circle(0.5, 0.5, 0.3),
        ellipse(0.5, 0.5, 0.3, 0.2),
        arrow(0.1, 0.1, 0.9, 0.9),
        polyline([(0.0, 0.0), (0.5, 0.5), (1.0, 0.0)]),
    ):
        assert isinstance(ss, StrokeSet)
        assert ss.stroke_count >= 1
        assert all(len(st) >= 1 for st in ss.strokes)


def test_compose_table_grid_counts() -> None:
    # 外框 4 + 内部竖 (c-1) + 内部横 (r-1)
    for rows, cols in ((1, 1), (2, 3), (4, 4), (3, 5)):
        ss = compose_table(rows, cols)
        expected = 4 + (cols - 1) + (rows - 1)
        assert ss.stroke_count == expected, (rows, cols, ss.stroke_count)


def test_compose_table_bbox_in_unit_box() -> None:
    ss = compose_table(3, 4, padding=0.05)
    box = ss.bbox()
    assert box is not None
    assert 0.04 <= box[0] <= 0.06
    assert 0.94 <= box[2] <= 0.96


def test_compose_table_without_source_ignores_text() -> None:
    # 离线（source=None）时只画空表，不应抛错
    texts = [["a", "b"], ["c", "d"]]
    ss = compose_table(2, 2, texts=texts, source=None)
    assert ss.stroke_count == 6  # 几何部分，与无文字一致


def test_compose_mindmap_counts() -> None:
    nodes = [
        MindNode("a", 0.3, 0.3, 0.2, 0.12),
        MindNode("b", 0.75, 0.25, 0.18, 0.1),
        MindNode("c", 0.7, 0.7, 0.18, 0.1),
    ]
    edges = [MindEdge("a", "b"), MindEdge("a", "c")]
    ss = compose_mindmap(nodes, edges)
    # 每个节点 4 笔边框 + 每条连线 1 笔
    assert ss.stroke_count == 4 * len(nodes) + len(edges)


def test_compose_mindmap_skips_unknown_edge() -> None:
    nodes = [MindNode("a", 0.5, 0.5, 0.2, 0.2)]
    edges = [MindEdge("a", "ghost")]
    ss = compose_mindmap(nodes, edges)
    assert ss.stroke_count == 4  # 仅边框，悬空连线被忽略


def test_svg_basic_shapes() -> None:
    svg = (
        '<svg viewBox="0 0 100 100">'
        '<line x1="0" y1="0" x2="10" y2="0"/>'
        '<rect x="10" y="10" width="20" height="20"/>'
        '<circle cx="50" cy="50" r="10"/>'
        '<polygon points="0,0 5,0 5,5"/>'
        "</svg>"
    )
    ss = svg_to_stroke_set(svg)
    # line + rect(闭合) + circle(闭合) + polygon(闭合)
    assert ss.stroke_count == 4
    box = ss.bbox()
    assert box is not None
    assert box[2] <= 1.0 and box[3] <= 1.0


def test_svg_path_with_arc_produces_points() -> None:
    # 半圆弧：起点 (10,80) 经弧到 (80,80)
    svg = '<svg viewBox="0 0 100 100"><path d="M10 80 A 35 35 0 0 1 80 80"/></svg>'
    ss = svg_to_stroke_set(svg)
    arc = ss.strokes[0]
    assert len(arc) > 4  # 弧长采样得到多个点


def test_svg_path_relative_and_closed() -> None:
    # 相对坐标 + 闭合：M0 0 l10 0 l0 10 z 应形成 4 个点的闭合折线
    svg = '<svg viewBox="0 0 100 100"><path d="M0 0 l10 0 l0 10 z"/></svg>'
    ss = svg_to_stroke_set(svg)
    stroke = ss.strokes[0]
    assert len(stroke) == 4  # 3 段 + 闭合回到起点


def test_svg_cubic_bezier_sampled() -> None:
    svg = '<svg viewBox="0 0 100 100"><path d="M0 0 C 10 0 10 10 20 10"/></svg>'
    ss = svg_to_stroke_set(svg)
    assert len(ss.strokes[0]) > 2


def test_svg_namespaced_and_bad_input() -> None:
    svg = '<svg xmlns="http://www.w3.org/2000/svg"><rect x="0" y="0" width="5" height="5"/></svg>'
    ss = svg_to_stroke_set(svg)
    assert ss.stroke_count == 1

    # 非法 XML 应抛 ValueError
    import pytest

    from handwriting.shapes import svg_to_stroke_set as parse

    with pytest.raises(ValueError):
        parse("<svg><unclosed>")
