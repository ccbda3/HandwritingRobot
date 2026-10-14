"""数据集读取与质量体检测试。"""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from handwriting.dataset import CaptureData

GOOD_PAYLOAD = {
    "format": "handwriting-capture/1.0",
    "meta": {
        "device": "触控笔 / 数位笔",
        "pressureSupported": True,
        "samplingHz": 150,
        "charCount": 150,
        "cellPx": 512,
        "coordSystem": "归一化到方格",
        "createdAt": "2026-10-14T23:59:00.000Z",
    },
    "samples": [
        {
            "index": 0,
            "char": "永",
            "rate": 150,
            "pressure": True,
            "strokes": [
                [[0.1, 0.1, 0.3, 0], [0.2, 0.2, 0.4, 8]],
                [[0.3, 0.3, 0.5, 0]],
            ],
        },
        {
            "index": 1,
            "char": "一",
            "rate": 148,
            "pressure": True,
            "strokes": [[[0.05, 0.5, 0.6, 0], [0.95, 0.5, 0.6, 60]]],
        },
    ],
}


class TestParsing:
    def test_reads_meta(self) -> None:
        data = CaptureData.from_dict(GOOD_PAYLOAD)
        assert data.meta.device == "触控笔 / 数位笔"
        assert data.meta.pressure_supported is True
        assert data.meta.sampling_hz == 150
        assert data.meta.cell_px == 512

    def test_counts(self) -> None:
        data = CaptureData.from_dict(GOOD_PAYLOAD)
        assert data.char_count == 2
        assert data.total_points == 5  # 永 3 点 + 一 2 点
        assert data.average_points_per_char == pytest.approx(2.5)

    def test_stroke_order_is_preserved(self) -> None:
        data = CaptureData.from_dict(GOOD_PAYLOAD)
        yong = data.by_char("永")[0]
        assert yong.stroke_count == 2
        assert yong.strokes[0].points[0].x == pytest.approx(0.1)
        assert yong.strokes[1].points[0].x == pytest.approx(0.3)

    def test_unknown_format_is_rejected(self) -> None:
        with pytest.raises(ValueError, match="不支持的数据格式"):
            CaptureData.from_dict({"format": "something/else", "samples": []})

    def test_missing_samples_is_rejected(self) -> None:
        with pytest.raises(ValueError, match="samples"):
            CaptureData.from_dict({"format": "handwriting-capture/1.0"})

    def test_load_from_file(self, tmp_path: Path) -> None:
        path = tmp_path / "sample.json"
        path.write_text(json.dumps(GOOD_PAYLOAD), encoding="utf-8")
        data = CaptureData.load(path)
        assert data.char_count == 2
        assert data.source == path


class TestQualityCheck:
    def test_good_dataset_passes(self) -> None:
        assert CaptureData.from_dict(GOOD_PAYLOAD).validate() == []

    def test_flags_missing_pressure(self) -> None:
        payload = json.loads(json.dumps(GOOD_PAYLOAD))
        payload["meta"]["pressureSupported"] = False
        problems = CaptureData.from_dict(payload).validate()
        assert any("压感" in p for p in problems)

    def test_flags_low_sampling_rate(self) -> None:
        payload = json.loads(json.dumps(GOOD_PAYLOAD))
        payload["meta"]["samplingHz"] = 60
        problems = CaptureData.from_dict(payload).validate()
        assert any("采样率偏低" in p for p in problems)

    def test_flags_empty_sample(self) -> None:
        payload = json.loads(json.dumps(GOOD_PAYLOAD))
        payload["samples"][0]["strokes"] = []
        problems = CaptureData.from_dict(payload).validate()
        assert any("没有笔画" in p for p in problems)


class TestStrokeSetConversion:
    def test_fit_normalises_into_unit_box(self) -> None:
        # 永 的样本字形范围是 (0.1, 0.1)~(0.3, 0.3)，等比缩放居中后
        # 应恰好落在 5% 内边距上
        data = CaptureData.from_dict(GOOD_PAYLOAD)
        stroke_set = data.by_char("永")[0].to_stroke_set(fit=True)
        assert stroke_set.bbox() == pytest.approx((0.05, 0.05, 0.95, 0.95))

    def test_fit_keeps_horizontal_stroke_horizontal(self) -> None:
        data = CaptureData.from_dict(GOOD_PAYLOAD)
        x0, y0, x1, y1 = data.by_char("一")[0].to_stroke_set(fit=True).bbox()
        assert y1 - y0 == pytest.approx(0.0)  # 横画高度为零，不应被拉伸
        assert x1 - x0 == pytest.approx(0.9)

    def test_iter_stroke_sets_matches_sample_count(self) -> None:
        data = CaptureData.from_dict(GOOD_PAYLOAD)
        assert len(list(data.iter_stroke_sets())) == data.char_count
