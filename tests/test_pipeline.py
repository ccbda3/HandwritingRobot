"""端到端链路集成测试。

验证「采集数据 → 风格提取 → 主动选字 → G 代码生成」这条完整路径，
而不是只测各个模块的内部行为。模块各自通过但串不起来，是这类项目最常见的失败模式。
"""

from __future__ import annotations

import json
import math
from pathlib import Path

import pytest

from handwriting.curriculum import COLD_START_CHARS, coverage_report, next_char
from handwriting.dataset import CaptureData
from handwriting.gcode import PlotSettings, strokes_to_gcode_with_stats
from handwriting.style import distance, extract, mean_features


def synthetic_char(index: int, char: str, slant: float = 0.0, strokes: int = 4) -> dict:
    """合成一个字形。

    笔画按「竖 / 横 / 斜」循环，而不是全用横线 —— 全是横线的话
    没有任何偏竖直的笔段，倾斜角特征恒为 0，测不出倾斜相关的行为。
    """
    stroke_list = []
    start_times = []
    clock = index * 30
    spread = max(1, strokes - 1)

    for s in range(strokes):
        kind = s % 3
        lane = 0.2 + 0.6 * (s / spread)
        points = []
        for p in range(12):
            ratio = p / 11.0
            pressure = round(0.25 + 0.5 * math.sin(ratio * math.pi), 4)
            if kind == 0:  # 竖：倾斜施加在竖直笔画上
                y = 0.15 + 0.7 * ratio
                x = lane + slant * (y - 0.5)
            elif kind == 1:  # 横
                x = 0.15 + 0.7 * ratio
                y = lane
            else:  # 斜（撇 / 捺）
                x = 0.2 + 0.6 * ratio
                y = 0.2 + 0.6 * ratio
            points.append([round(x, 5), round(y, 5), pressure, p * 8])
        stroke_list.append(points)
        start_times.append(clock)
        clock += 120

    return {
        "index": index,
        "char": char,
        "strokeCount": strokes,
        "rate": 150,
        "pressure": True,
        "strokes": stroke_list,
        "strokeStartT": start_times,
    }


def synthetic_capture(chars: list[str], slant: float = 0.0, strokes: int = 4) -> dict:
    return {
        "format": "handwriting-capture/1.0",
        "meta": {
            "device": "触控笔 / 数位笔",
            "pressureSupported": True,
            "samplingHz": 150,
            "charCount": len(chars),
            "cellPx": 512,
            "devicePixelRatio": 2.0,
            "coordSystem": "归一化到方格",
            "orderStrategy": "test",
            "createdAt": "2026-10-15T00:00:00.000Z",
        },
        "samples": [synthetic_char(i, char, slant, strokes) for i, char in enumerate(chars)],
    }


class TestCaptureToStyle:
    def test_quality_check_passes_on_synthetic_data(self) -> None:
        data = CaptureData.from_dict(synthetic_capture(list(COLD_START_CHARS)))
        assert data.validate() == []

    def test_every_sample_yields_features(self) -> None:
        data = CaptureData.from_dict(synthetic_capture(list(COLD_START_CHARS)))
        for sample in data.samples:
            features = extract(
                sample.to_stroke_set(fit=True),
                start_times=None,
            )
            assert features.stroke_count == sample.stroke_count
            assert features.pressure_mean is not None
            assert features.slant_deg == pytest.approx(0.0, abs=1.0)

    def test_start_times_enable_pause_ratio(self) -> None:
        data = CaptureData.from_dict(synthetic_capture(["永"]))
        sample = data.samples[0]
        start_times = sample.stroke_start_t
        assert start_times is not None

        with_pause = extract(sample.to_stroke_set(), start_times=start_times)
        without = extract(sample.to_stroke_set())
        assert with_pause.pause_ratio is not None
        assert without.pause_ratio is None
        # 每笔 88 ms（11 段 × 8 ms），笔间 32 ms => 抬笔占比约 0.27
        assert 0.1 < with_pause.pause_ratio < 0.5

    def test_writer_style_is_recoverable_from_average(self) -> None:
        """同一书写者的平均风格向量，应当比不同书写者之间更接近。

        这是整套主动选字与质量评估的前提假设 —— 如果它不成立，
        后面的风格距离指标就没有意义。
        """
        chars = list(COLD_START_CHARS)

        upright = CaptureData.from_dict(synthetic_capture(chars, slant=0.0))
        upright_again = CaptureData.from_dict(synthetic_capture(chars, slant=0.02))
        leaning = CaptureData.from_dict(synthetic_capture(chars, slant=0.35))

        def average(data: CaptureData):
            return mean_features([extract(s.to_stroke_set(fit=True)) for s in data.samples])

        base = average(upright)
        near = average(upright_again)
        far = average(leaning)

        assert distance(base, near) < distance(base, far), (
            "轻微倾斜的样本应当比大幅倾斜的样本更接近基准风格"
        )

    def test_slant_ordering_is_monotonic(self) -> None:
        chars = list(COLD_START_CHARS)
        slants = [0.0, 0.15, 0.3]
        means = []
        for slant in slants:
            data = CaptureData.from_dict(synthetic_capture(chars, slant=slant))
            features = mean_features([extract(s.to_stroke_set(fit=True)) for s in data.samples])
            means.append(features.slant_deg)
        assert means[0] < means[1] < means[2]


class TestStyleToGcode:
    def test_full_page_program_is_well_formed(self) -> None:
        chars = list(COLD_START_CHARS)
        data = CaptureData.from_dict(synthetic_capture(chars))
        rows = [[sample.to_stroke_set(fit=True) for sample in data.samples]]

        result = strokes_to_gcode_with_stats(rows, PlotSettings())
        lines = result.lines_text().strip().splitlines()

        assert lines[0] == "G21"
        assert lines[1] == "G90"
        assert lines[-1] == "G0 X0 Y0"
        assert result.stats.stroke_count == len(chars) * 4
        assert result.stats.point_count == len(chars) * 4 * 12
        assert result.stats.draw_length_mm > 0

    def test_stroke_order_survives_the_pipeline(self) -> None:
        """笔顺是整套系统的核心信息，必须能从采集数据一路带到 G 代码。"""
        data = CaptureData.from_dict(synthetic_capture(["永"], strokes=4))
        sample = data.samples[0]
        rows = [[sample.to_stroke_set(fit=True)]]

        lines = strokes_to_gcode_with_stats(rows, PlotSettings()).lines_text().splitlines()
        pen_down_indexes = [i for i, line in enumerate(lines) if line == "G0 Z0"]
        assert len(pen_down_indexes) == 4  # 四笔各自独立落笔，顺序与采集一致

    def test_row_layout_places_chars_side_by_side(self) -> None:
        data = CaptureData.from_dict(synthetic_capture(["永", "西"]))
        rows = [[sample.to_stroke_set(fit=True) for sample in data.samples]]
        lines = strokes_to_gcode_with_stats(rows, PlotSettings()).lines_text().splitlines()

        travels = [line for line in lines if line.startswith("G0 X") and line != "G0 X0 Y0"]
        first_x = float(travels[0].split()[1][1:])
        second_x = float(travels[4].split()[1][1:])  # 第二个字的首笔
        assert second_x > first_x


class TestCaptureFileRoundTrip:
    def test_write_then_load(self, tmp_path: Path) -> None:
        payload = synthetic_capture(list(COLD_START_CHARS))
        path = tmp_path / "capture.json"
        path.write_text(json.dumps(payload, ensure_ascii=False), encoding="utf-8")

        data = CaptureData.load(path)
        assert data.char_count == len(COLD_START_CHARS)
        assert data.meta.pressure_supported is True
        assert data.source == path
        assert [s.char for s in data.samples] == list(COLD_START_CHARS)


class TestCurriculumDrivesCollection:
    def test_recommendation_sequence_matches_cold_start(self) -> None:
        collected: list[str] = []
        for expected in COLD_START_CHARS:
            assert next_char(collected) == expected
            collected.append(expected)

    def test_report_after_cold_start_shows_remaining_gaps(self) -> None:
        report = coverage_report(list(COLD_START_CHARS))
        assert report["collected"] == len(COLD_START_CHARS)
        assert len(report["missing_cells"]) == 3  # 11 个格子减去冷启动覆盖的 8 个
        assert report["milestone"] == 40
