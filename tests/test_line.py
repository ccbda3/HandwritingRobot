"""整行数据集读取、切分与质量体检测试。"""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from handwriting.line import LineCapture

GOOD_PAYLOAD = {
    "format": "handwriting-capture/2.0",
    "meta": {
        "device": "数位板",
        "pressureSupported": True,
        "samplingHz": 150,
        "lineCount": 1,
        "coordSystem": "归一化到整行方框",
        "createdAt": "2026-10-16T00:00:00.000Z",
    },
    "lines": [
        {
            "index": 0,
            "text": "一二三",
            "rate": 150,
            "pressure": True,
            "strokes": [
                [[0.05, 0.50, 0.6, 0], [0.25, 0.50, 0.6, 100]],
                [[0.30, 0.40, 0.5, 0], [0.45, 0.40, 0.5, 100]],
                [[0.30, 0.60, 0.5, 0], [0.45, 0.60, 0.5, 100]],
                [[0.55, 0.30, 0.6, 0], [0.75, 0.30, 0.6, 100]],
                [[0.55, 0.50, 0.6, 0], [0.75, 0.50, 0.6, 100]],
                [[0.55, 0.70, 0.6, 0], [0.75, 0.70, 0.6, 100]],
            ],
            "strokeStartT": [0, 400, 510, 910, 1020, 1130],
            "charRanges": [[0, 1], [1, 3], [3, 6]],
        }
    ],
}


class TestParsing:
    def test_reads_meta(self) -> None:
        data = LineCapture.from_dict(GOOD_PAYLOAD)
        assert data.meta.device == "数位板"
        assert data.meta.pressure_supported is True
        assert data.meta.sampling_hz == 150

    def test_counts(self) -> None:
        data = LineCapture.from_dict(GOOD_PAYLOAD)
        assert data.line_count == 1
        assert data.total_strokes == 6
        assert data.total_points == 12
        assert data.total_chars == 3

    def test_unknown_format_is_rejected(self) -> None:
        with pytest.raises(ValueError, match="不支持的数据格式"):
            LineCapture.from_dict({"format": "handwriting-capture/1.0", "lines": []})

    def test_missing_lines_is_rejected(self) -> None:
        with pytest.raises(ValueError, match="lines"):
            LineCapture.from_dict({"format": "handwriting-capture/2.0"})

    def test_load_from_file(self, tmp_path: Path) -> None:
        path = tmp_path / "line.json"
        path.write_text(json.dumps(GOOD_PAYLOAD), encoding="utf-8")
        data = LineCapture.load(path)
        assert data.line_count == 1
        assert data.source == path


class TestCharRanges:
    def test_char_start_times(self) -> None:
        line = LineCapture.from_dict(GOOD_PAYLOAD).lines[0]
        assert line.char_start_times() == [0, 400, 910]

    def test_split_chars_keeps_positions(self) -> None:
        line = LineCapture.from_dict(GOOD_PAYLOAD).lines[0]
        chars = line.split_chars()
        assert [c for c, _ in chars] == ["一", "二", "三"]
        assert [s.stroke_count for _, s in chars] == [1, 2, 3]
        # 不 fit 时保留行内原始坐标：三 的首笔 x 约 0.55
        assert chars[2][1].strokes[0].points[0].x == pytest.approx(0.55)

    def test_split_chars_fit_normalises(self) -> None:
        line = LineCapture.from_dict(GOOD_PAYLOAD).lines[0]
        chars = dict(line.split_chars(fit=True))
        # 一 是横画，fit 后宽度铺满 90%，高度为零（不被拉伸）
        x0, y0, x1, y1 = chars["一"].bbox()
        assert x1 - x0 == pytest.approx(0.9)
        assert y1 - y0 == pytest.approx(0.0)

    def test_missing_ranges_cannot_split(self) -> None:
        payload = json.loads(json.dumps(GOOD_PAYLOAD))
        del payload["lines"][0]["charRanges"]
        line = LineCapture.from_dict(payload).lines[0]
        assert line.char_ranges is None
        with pytest.raises(ValueError, match="char_ranges"):
            line.split_chars()

    def test_segment_fills_ranges(self) -> None:
        payload = json.loads(json.dumps(GOOD_PAYLOAD))
        del payload["lines"][0]["charRanges"]
        line = LineCapture.from_dict(payload).lines[0]
        assert line.segment() == [(0, 1), (1, 3), (3, 6)]
        assert line.char_ranges == [(0, 1), (1, 3), (3, 6)]


class TestQualityCheck:
    def test_good_dataset_passes(self) -> None:
        assert LineCapture.from_dict(GOOD_PAYLOAD).validate() == []

    def test_flags_missing_pressure(self) -> None:
        payload = json.loads(json.dumps(GOOD_PAYLOAD))
        payload["meta"]["pressureSupported"] = False
        problems = LineCapture.from_dict(payload).validate()
        assert any("压感" in p for p in problems)

    def test_flags_low_sampling_rate(self) -> None:
        payload = json.loads(json.dumps(GOOD_PAYLOAD))
        payload["meta"]["samplingHz"] = 60
        problems = LineCapture.from_dict(payload).validate()
        assert any("采样率偏低" in p for p in problems)

    def test_flags_start_time_mismatch(self) -> None:
        payload = json.loads(json.dumps(GOOD_PAYLOAD))
        payload["lines"][0]["strokeStartT"] = [0, 400]
        problems = LineCapture.from_dict(payload).validate()
        assert any("strokeStartT" in p for p in problems)

    def test_flags_bad_char_ranges(self) -> None:
        payload = json.loads(json.dumps(GOOD_PAYLOAD))
        payload["lines"][0]["charRanges"] = [[0, 1], [1, 3]]  # 只覆盖了前 3 笔
        problems = LineCapture.from_dict(payload).validate()
        assert any("charRanges" in p for p in problems)

    def test_flags_empty_line(self) -> None:
        payload = json.loads(json.dumps(GOOD_PAYLOAD))
        payload["lines"][0]["strokes"] = []
        problems = LineCapture.from_dict(payload).validate()
        assert any("没有笔画" in p for p in problems)
