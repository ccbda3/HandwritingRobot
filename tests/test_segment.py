"""整行「字」分段测试。"""

from __future__ import annotations

from itertools import pairwise

import pytest

from handwriting.segment import segment_by_gap, segment_line, stroke_gaps
from handwriting.stroke import Point, Stroke


def build(specs: list[tuple[int, int]]) -> tuple[list[Stroke], list[int]]:
    """按 (起笔时刻, 持续时长) 构造笔画；笔画内部时间从 0 到 duration。"""
    strokes: list[Stroke] = []
    starts: list[int] = []
    for start_t, duration in specs:
        strokes.append(Stroke([Point(0.0, 0.0, 0.5, 0), Point(0.5, 0.0, 0.5, duration)]))
        starts.append(start_t)
    return strokes, starts


class TestStrokeGaps:
    def test_small_and_large_gaps(self) -> None:
        strokes, starts = build([(0, 100), (110, 100), (400, 100)])
        assert stroke_gaps(strokes, starts) == [10.0, 190.0]

    def test_overlap_is_clamped_to_zero(self) -> None:
        strokes, starts = build([(0, 100), (50, 100)])
        assert stroke_gaps(strokes, starts) == [0.0]

    def test_length_mismatch_rejected(self) -> None:
        strokes, _ = build([(0, 100)])
        with pytest.raises(ValueError, match="长度不一致"):
            stroke_gaps(strokes, [0, 1])


class TestSegmentByGap:
    def test_splits_on_large_gap(self) -> None:
        strokes, starts = build([(0, 100), (400, 100), (510, 100)])
        assert segment_by_gap(strokes, starts) == [(0, 1), (1, 3)]

    def test_no_gap_means_single_segment(self) -> None:
        strokes, starts = build([(0, 100), (110, 100), (220, 100)])
        assert segment_by_gap(strokes, starts) == [(0, 3)]

    def test_custom_threshold_ignores_medium_gap(self) -> None:
        strokes, starts = build([(0, 100), (400, 100), (510, 100)])
        assert segment_by_gap(strokes, starts, gap_ms=350.0) == [(0, 3)]

    def test_empty(self) -> None:
        assert segment_by_gap([], []) == []


class TestSegmentLine:
    def test_exact_when_gaps_are_clear(self) -> None:
        # 一(1笔) 二(2笔) 三(3笔)：字间 300ms、字内 10ms
        strokes, starts = build(
            [(0, 100), (400, 100), (510, 100), (910, 100), (1020, 100), (1130, 100)]
        )
        assert segment_line(strokes, starts, "一二三") == [(0, 1), (1, 3), (3, 6)]

    def test_merges_extra_segment(self) -> None:
        # 三 中间混入一个 200ms 的停顿，粗分成 4 段；应合并间隔最小的那一对
        strokes, starts = build(
            [(0, 100), (400, 100), (510, 100), (910, 100), (1210, 100), (1320, 100)]
        )
        assert segment_line(strokes, starts, "一二三") == [(0, 1), (1, 3), (3, 6)]

    def test_splits_when_too_few_segments(self) -> None:
        # 三个单笔字、间隔都很小，粗分只有 1 段；应按字数拆成 3 段
        strokes, starts = build([(0, 100), (110, 100), (220, 100)])
        assert segment_line(strokes, starts, "一一一") == [(0, 1), (1, 2), (2, 3)]

    def test_result_partitions_all_strokes(self) -> None:
        strokes, starts = build(
            [(0, 100), (400, 100), (510, 100), (910, 100), (1020, 100), (1130, 100)]
        )
        ranges = segment_line(strokes, starts, "一二三")
        assert ranges[0][0] == 0
        assert ranges[-1][1] == len(strokes)
        for (_, end), (nxt, _) in pairwise(ranges):
            assert end == nxt  # 连续、无缝隙

    def test_more_chars_than_strokes_rejected(self) -> None:
        strokes, starts = build([(0, 100), (400, 100)])
        with pytest.raises(ValueError, match="多于笔画数"):
            segment_line(strokes, starts, "一二三")

    def test_empty_text_returns_empty(self) -> None:
        strokes, starts = build([(0, 100)])
        assert segment_line(strokes, starts, "") == []

    def test_no_strokes_rejected(self) -> None:
        with pytest.raises(ValueError, match="没有笔画"):
            segment_line([], [], "一")
