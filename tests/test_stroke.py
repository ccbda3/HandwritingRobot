"""轨迹数据结构测试。"""

from __future__ import annotations

import pytest

from handwriting.stroke import Point, Stroke, StrokeSet, strokes_from_arrays


def diagonal_stroke() -> Stroke:
    return Stroke([Point(0.0, 0.0), Point(0.3, 0.4), Point(0.3, 0.8)])


class TestPoint:
    def test_rejects_non_finite_coordinate(self) -> None:
        with pytest.raises(ValueError, match="有限数值"):
            Point(float("nan"), 0.0)

    def test_rejects_pressure_out_of_range(self) -> None:
        with pytest.raises(ValueError, match="pressure"):
            Point(0.0, 0.0, pressure=1.5)

    def test_rejects_negative_timestamp(self) -> None:
        with pytest.raises(ValueError, match="不能为负"):
            Point(0.0, 0.0, t=-1)

    def test_as_tuple(self) -> None:
        assert Point(0.1, 0.2, 0.3, 4).as_tuple() == (0.1, 0.2, 0.3, 4)

    def test_defaults(self) -> None:
        point = Point(0.5, 0.5)
        assert point.pressure == 0.5
        assert point.t == 0


class TestStroke:
    def test_empty_stroke_is_rejected(self) -> None:
        with pytest.raises(ValueError, match="至少需要一个采样点"):
            Stroke([])

    def test_length_sums_segments(self) -> None:
        assert diagonal_stroke().length == pytest.approx(0.9)

    def test_duration_and_pressure(self) -> None:
        stroke = Stroke([Point(0.0, 0.0, 0.2, 0), Point(0.1, 0.0, 0.8, 40)])
        assert stroke.duration_ms == 40
        assert stroke.mean_pressure == pytest.approx(0.5)

    def test_bbox(self) -> None:
        assert diagonal_stroke().bbox() == (0.0, 0.0, 0.3, 0.8)

    def test_translate_and_scale(self) -> None:
        moved = Stroke([Point(0.0, 0.0)]).translated(0.25, -0.5)
        assert moved.points[0].x == pytest.approx(0.25)
        assert moved.points[0].y == pytest.approx(-0.5)

        scaled = Stroke([Point(0.5, 0.5)]).scaled(2.0, 2.0)
        assert scaled.points[0].x == pytest.approx(1.0)

    def test_shear_shifts_x_by_y(self) -> None:
        sheared = Stroke([Point(0.0, 1.0)]).sheared(kx=0.2)
        assert sheared.points[0].x == pytest.approx(0.2)
        assert sheared.points[0].y == pytest.approx(1.0)

    def test_iteration_and_len(self) -> None:
        stroke = diagonal_stroke()
        assert len(stroke) == 3
        assert [p.x for p in stroke] == [0.0, 0.3, 0.3]


class TestStrokeSet:
    def test_aggregate_counts(self) -> None:
        char = StrokeSet([diagonal_stroke(), Stroke([Point(0.5, 0.5)])], "永")
        assert char.stroke_count == 2
        assert char.point_count == 4
        assert char.total_length == pytest.approx(0.9)

    def test_bbox_of_empty_set_is_none(self) -> None:
        assert StrokeSet([]).bbox() is None

    def test_bbox_unions_all_strokes(self) -> None:
        char = StrokeSet([Stroke([Point(0.2, 0.2)]), Stroke([Point(0.9, 0.7)])])
        assert char.bbox() == (0.2, 0.2, 0.9, 0.7)

    def test_fit_to_box_fills_unit_square(self) -> None:
        char = StrokeSet([Stroke([Point(0.2, 0.3), Point(0.8, 0.9)])])
        box = char.fit_to_box(1.0, padding=0.0).bbox()
        assert box == pytest.approx((0.0, 0.0, 1.0, 1.0))

    def test_fit_to_box_honours_padding(self) -> None:
        char = StrokeSet([Stroke([Point(0.2, 0.3), Point(0.8, 0.9)])])
        box = char.fit_to_box(1.0, padding=0.1).bbox()
        assert box == pytest.approx((0.1, 0.1, 0.9, 0.9))

    def test_fit_to_box_preserves_aspect_ratio(self) -> None:
        # 宽高比 1:2 的字形，缩放后必须保持，不能拉伸
        char = StrokeSet([Stroke([Point(0.0, 0.0), Point(0.4, 0.8)])])
        x0, y0, x1, y1 = char.fit_to_box(1.0, padding=0.0).bbox()
        assert (x1 - x0) / (y1 - y0) == pytest.approx(0.5)
        assert y1 - y0 == pytest.approx(1.0)

    def test_fit_to_box_on_degenerate_glyph_returns_empty(self) -> None:
        fitted = StrokeSet([Stroke([Point(0.5, 0.5)])]).fit_to_box()
        assert fitted.stroke_count == 0

    def test_flatten_keeps_writing_order(self) -> None:
        char = StrokeSet([Stroke([Point(0.1, 0.1), Point(0.2, 0.2)]), Stroke([Point(0.3, 0.3)])])
        assert [p.x for p in char.flatten()] == [0.1, 0.2, 0.3]


class TestStrokesFromArrays:
    def test_builds_strokes_and_skips_empty(self) -> None:
        strokes = strokes_from_arrays([[[0.1, 0.2, 0.3, 0], [0.4, 0.5, 0.6, 12]], []])
        assert len(strokes) == 1
        assert strokes[0].points[1].t == 12
        assert strokes[0].points[1].pressure == pytest.approx(0.6)

    def test_accepts_empty_input(self) -> None:
        assert strokes_from_arrays([]) == []
