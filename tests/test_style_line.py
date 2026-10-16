"""行级风格特征测试。"""

from __future__ import annotations

import copy

import pytest

from handwriting.line import LineCapture
from handwriting.stroke import Point, Stroke
from handwriting.style import LineStyleFeatures, distance, extract_line, mean_line_features


def hstroke(x0: float, x1: float, y: float, start_t: int, dur: int = 50) -> Stroke:
    """一条水平笔画，内部时间 0..dur。"""
    return Stroke([Point(x0, y, 0.5, 0), Point(x1, y, 0.5, dur)])


def build_line(
    boxes: list[tuple[float, float, float]], starts: list[int]
) -> tuple[list[Stroke], list[tuple[int, int]], list[int]]:
    """boxes 为每个字的 (x0, x1, y)，starts 为每个字的起笔时刻。每字一笔。"""
    strokes: list[Stroke] = []
    ranges: list[tuple[int, int]] = []
    start_t: list[int] = []
    for i, ((x0, x1, y), st) in enumerate(zip(boxes, starts, strict=True)):
        strokes.append(hstroke(x0, x1, y, st))
        ranges.append((i, i + 1))
        start_t.append(st)
    return strokes, ranges, start_t


def feats(**over: object) -> LineStyleFeatures:
    base: dict[str, object] = {
        "char_count": 3,
        "char_width_cv": 0.1,
        "char_height_cv": 0.1,
        "char_gap_mean": 0.1,
        "char_gap_cv": 0.1,
        "baseline_drift": 0.01,
        "baseline_slope": 0.0,
        "char_pause_ratio": 0.2,
        "connection_rate": 0.1,
        "slant_deg": 0.0,
    }
    base.update(over)
    return LineStyleFeatures(**base)  # type: ignore[arg-type]


class TestExtractLine:
    def test_regular_line(self) -> None:
        strokes, ranges, starts = build_line(
            [(0.0, 0.1, 0.5), (0.2, 0.3, 0.5), (0.4, 0.5, 0.5)], [0, 200, 400]
        )
        f = extract_line(strokes, ranges, starts)
        assert f.char_count == 3
        assert f.char_width_cv == pytest.approx(0.0)
        assert f.char_gap_mean == pytest.approx(0.1)
        assert f.char_gap_cv == pytest.approx(0.0)
        assert f.baseline_drift == pytest.approx(0.0)
        assert f.baseline_slope == pytest.approx(0.0)
        assert f.slant_deg == pytest.approx(0.0)
        assert f.connection_rate == pytest.approx(0.0)  # 间隔 150ms > 15ms
        assert f.char_pause_ratio == pytest.approx(300.0 / 450.0)
        # 全是横画，字高恒为 0 → 字高变异系数不可用
        assert f.char_height_cv is None

    def test_width_variation_is_positive(self) -> None:
        strokes, ranges, starts = build_line(
            [(0.0, 0.1, 0.5), (0.2, 0.4, 0.5), (0.5, 0.6, 0.5)], [0, 200, 400]
        )
        f = extract_line(strokes, ranges, starts)
        assert f.char_width_cv is not None and f.char_width_cv > 0.0

    def test_baseline_drift_and_slope(self) -> None:
        strokes, ranges, starts = build_line(
            [(0.0, 0.1, 0.4), (0.2, 0.3, 0.5), (0.4, 0.5, 0.6)], [0, 200, 400]
        )
        f = extract_line(strokes, ranges, starts)
        assert f.baseline_drift == pytest.approx(0.0816496, rel=1e-4)
        assert f.baseline_slope == pytest.approx(0.1)

    def test_connection_rate_all_connected(self) -> None:
        strokes, ranges, starts = build_line([(0.0, 0.1, 0.5), (0.2, 0.3, 0.5)], [0, 50])
        f = extract_line(strokes, ranges, starts)
        assert f.connection_rate == pytest.approx(1.0)
        assert f.char_pause_ratio == pytest.approx(0.0)

    def test_connection_rate_partial(self) -> None:
        strokes, ranges, starts = build_line(
            [(0.0, 0.1, 0.5), (0.2, 0.3, 0.5), (0.4, 0.5, 0.5)], [0, 50, 250]
        )
        f = extract_line(strokes, ranges, starts)
        assert f.connection_rate == pytest.approx(0.5)

    def test_single_char_has_undefined_line_metrics(self) -> None:
        strokes, ranges, starts = build_line([(0.0, 0.1, 0.5)], [0])
        f = extract_line(strokes, ranges, starts)
        assert f.char_count == 1
        assert f.char_width_cv is None
        assert f.char_gap_mean is None
        assert f.char_gap_cv is None
        assert f.baseline_drift is None
        assert f.baseline_slope is None
        assert f.char_pause_ratio is None
        assert f.connection_rate is None

    def test_length_mismatch_rejected(self) -> None:
        strokes, ranges, _ = build_line([(0.0, 0.1, 0.5), (0.2, 0.3, 0.5)], [0, 200])
        with pytest.raises(ValueError, match="长度不一致"):
            extract_line(strokes, ranges, [0])

    def test_missing_char_ranges_rejected(self) -> None:
        strokes, _, starts = build_line([(0.0, 0.1, 0.5)], [0])
        with pytest.raises(ValueError, match="char_ranges"):
            extract_line(strokes, [], starts)

    def test_empty_rejected(self) -> None:
        with pytest.raises(ValueError, match="空整行"):
            extract_line([], [], [])


class TestVectorAndDistance:
    def test_availability_masks_none(self) -> None:
        f = feats(char_width_cv=None)
        avail = dict(zip(f.numeric_fields, f.availability(), strict=True))
        assert avail["char_width_cv"] is False
        assert avail["connection_rate"] is True
        assert f.to_vector()[f.numeric_fields.index("char_width_cv")] == 0.0

    def test_distance_zero_for_identical(self) -> None:
        assert distance(feats(), feats()) == pytest.approx(0.0)

    def test_distance_positive_when_different(self) -> None:
        assert distance(feats(char_width_cv=0.0), feats(char_width_cv=0.3)) > 0.0

    def test_mean_line_features_intersects_availability(self) -> None:
        merged = mean_line_features([feats(char_width_cv=0.1), feats(char_width_cv=None)])
        assert merged.char_width_cv is None  # 一行缺失 → 整体不可用
        assert merged.connection_rate == pytest.approx(0.1)
        assert merged.char_count == 3


LINE_PAYLOAD = {
    "format": "handwriting-capture/2.0",
    "meta": {"pressureSupported": True, "samplingHz": 150},
    "lines": [
        {
            "index": 0,
            "text": "一二三",
            "rate": 150,
            "pressure": True,
            "strokes": [
                [[0.00, 0.50, 0.5, 0], [0.10, 0.50, 0.5, 50]],
                [[0.20, 0.50, 0.5, 0], [0.30, 0.50, 0.5, 50]],
                [[0.40, 0.50, 0.5, 0], [0.50, 0.50, 0.5, 50]],
            ],
            "strokeStartT": [0, 200, 400],
            "charRanges": [[0, 1], [1, 2], [2, 3]],
        }
    ],
}


class TestLineSampleIntegration:
    def test_style_features_from_payload(self) -> None:
        line = LineCapture.from_dict(LINE_PAYLOAD).lines[0]
        f = line.style_features()
        assert f.char_count == 3
        assert f.char_gap_mean == pytest.approx(0.1)
        assert f.connection_rate == pytest.approx(0.0)

    def test_style_features_auto_segments(self) -> None:
        payload = copy.deepcopy(LINE_PAYLOAD)
        del payload["lines"][0]["charRanges"]
        line = LineCapture.from_dict(payload).lines[0]
        f = line.style_features()
        assert f.char_count == 3  # 抬笔间隔 150ms 自动切出 3 段
