"""曲线插值与重采样测试。"""

from __future__ import annotations

import math
from itertools import pairwise

import pytest

from handwriting.curve import catmull_rom, resample, smooth_and_resample
from handwriting.stroke import Point


def polyline(*coords: tuple[float, float]) -> list[Point]:
    return [Point(x, y, 0.5, 0) for x, y in coords]


class TestResample:
    def test_uniform_spacing(self) -> None:
        result = resample(polyline((0.0, 0.0), (1.0, 0.0)), step=0.05)
        gaps = [b.x - a.x for a, b in pairwise(result)]
        for gap in gaps[:-1]:  # 末段是残段
            assert gap == pytest.approx(0.05, abs=1e-9)

    def test_endpoints_preserved(self) -> None:
        result = resample(polyline((0.1, 0.2), (0.9, 0.2)), step=0.05)
        assert result[0].x == pytest.approx(0.1)
        assert result[-1].x == pytest.approx(0.9)

    def test_multi_segment_path(self) -> None:
        # L 形：横 0.5 + 竖 0.5 = 总长 1.0，步长 0.1 应得到约 11 个点
        result = resample(polyline((0.0, 0.0), (0.5, 0.0), (0.5, 0.5)), step=0.1)
        assert 10 <= len(result) <= 12

    def test_short_stroke_keeps_both_points(self) -> None:
        result = resample(polyline((0.0, 0.0), (0.01, 0.0)), step=0.1)
        assert len(result) == 2

    def test_degenerate_segments_are_skipped(self) -> None:
        # 重复点不应产生除零或空步进
        result = resample(polyline((0.0, 0.0), (0.0, 0.0), (0.5, 0.0)), step=0.1)
        assert result[0].x == pytest.approx(0.0)
        assert result[-1].x == pytest.approx(0.5)

    def test_pressure_and_time_are_interpolated(self) -> None:
        points = [Point(0.0, 0.0, 0.0, 0), Point(1.0, 0.0, 1.0, 100)]
        result = resample(points, step=0.25)
        middle = result[len(result) // 2]
        assert 0.0 < middle.pressure < 1.0
        assert 0 < middle.t < 100

    def test_single_point_is_returned_as_is(self) -> None:
        points = [Point(0.5, 0.5)]
        assert resample(points, 0.1) == points

    def test_rejects_non_positive_step(self) -> None:
        with pytest.raises(ValueError, match="step 必须为正"):
            resample(polyline((0.0, 0.0), (1.0, 0.0)), step=0.0)


class TestCatmullRom:
    def test_passes_through_all_control_points(self) -> None:
        """Catmull-Rom 的关键性质：曲线通过每一个控制点。

        选它而不是 B 样条就是为了这一点 —— 字形中心线的控制点本身
        落在笔画上，被平滑掉意味着笔画走偏。
        """
        control = polyline((0.0, 0.0), (0.3, 0.4), (0.6, 0.2), (1.0, 0.8))
        result = catmull_rom(control, samples_per_segment=8)
        for index, point in enumerate(control):
            position = index * 8
            assert result[position].x == pytest.approx(point.x)
            assert result[position].y == pytest.approx(point.y)

    def test_sample_count(self) -> None:
        # n 个控制点 → (n-1) 段；非末段各取 samples 个，末段多取一个收尾点
        control = polyline((0.0, 0.0), (0.5, 0.0), (1.0, 0.0))
        result = catmull_rom(control, samples_per_segment=4)
        assert len(result) == 4 + 5

    def test_endpoints_are_not_extrapolated(self) -> None:
        # 端点用重复法处理，曲线不应外扩到控制点之外
        control = polyline((0.0, 0.0), (0.5, 0.0), (1.0, 0.0))
        result = catmull_rom(control, samples_per_segment=10)
        xs = [p.x for p in result]
        assert min(xs) == pytest.approx(0.0, abs=1e-9)
        assert max(xs) == pytest.approx(1.0, abs=1e-9)

    def test_collinear_points_stay_collinear(self) -> None:
        control = polyline((0.0, 0.5), (0.25, 0.5), (0.5, 0.5), (1.0, 0.5))
        result = catmull_rom(control, samples_per_segment=6)
        for point in result:
            assert point.y == pytest.approx(0.5, abs=1e-9)

    def test_curve_is_smoother_than_polyline(self) -> None:
        """折线在拐点处方向突变，样条应当把它摊开成若干小转角。"""
        control = polyline((0.0, 0.0), (0.5, 0.0), (0.5, 0.5))

        def max_turn(points: list[Point]) -> float:
            worst = 0.0
            for i in range(1, len(points) - 1):
                ax, ay = points[i].x - points[i - 1].x, points[i].y - points[i - 1].y
                bx, by = points[i + 1].x - points[i].x, points[i + 1].y - points[i].y
                na, nb = math.hypot(ax, ay), math.hypot(bx, by)
                if na < 1e-9 or nb < 1e-9:
                    continue
                cosine = max(-1.0, min(1.0, (ax * bx + ay * by) / (na * nb)))
                worst = max(worst, math.degrees(math.acos(cosine)))
            return worst

        smoothed = catmull_rom(control, samples_per_segment=20)
        assert max_turn(smoothed) < max_turn(control) - 30.0

    def test_two_points_is_a_straight_line(self) -> None:
        control = polyline((0.0, 0.0), (1.0, 0.0))
        result = catmull_rom(control, samples_per_segment=5)
        assert len(result) == 6
        for point in result:
            assert point.y == pytest.approx(0.0, abs=1e-9)

    def test_single_point_is_returned_as_is(self) -> None:
        points = [Point(0.5, 0.5)]
        assert catmull_rom(points) == points

    def test_rejects_zero_samples(self) -> None:
        with pytest.raises(ValueError, match="samples_per_segment"):
            catmull_rom(polyline((0.0, 0.0), (1.0, 0.0)), samples_per_segment=0)


class TestSmoothAndResample:
    def test_output_is_uniformly_spaced(self) -> None:
        """重采样保证的是**弧长**均匀，不是弦长均匀。

        路径弯曲时相邻两点的直线距离必然小于弧长步长，弯曲越剧烈差得越多。
        因此断言：弦长一律不超过步长，且**中位数**贴近步长
        （中位数比逐点公差更稳健，又足以抓住「步长算错」这类真问题）。
        """
        control = polyline((0.0, 0.0), (0.3, 0.4), (0.6, 0.2), (1.0, 0.8))
        result = smooth_and_resample(control, step=0.02)
        gaps = sorted(math.hypot(b.x - a.x, b.y - a.y) for a, b in pairwise(result))

        assert all(gap <= 0.02 + 1e-9 for gap in gaps)
        assert gaps[len(gaps) // 2] == pytest.approx(0.02, rel=0.005)

    def test_smoothing_happens_before_resampling(self) -> None:
        """顺序不能颠倒：先重采样会把控制点之间的直线段固化。

        用「拐点是否被摊开」判断顺序是否正确 —— 若先重采样，
        L 形拐角会保留一个接近 90° 的突变。
        """
        control = polyline((0.0, 0.0), (0.5, 0.0), (0.5, 0.5))
        result = smooth_and_resample(control, step=0.05)

        worst = 0.0
        for i in range(1, len(result) - 1):
            ax, ay = result[i].x - result[i - 1].x, result[i].y - result[i - 1].y
            bx, by = result[i + 1].x - result[i].x, result[i + 1].y - result[i].y
            na, nb = math.hypot(ax, ay), math.hypot(bx, by)
            if na < 1e-9 or nb < 1e-9:
                continue
            cosine = max(-1.0, min(1.0, (ax * bx + ay * by) / (na * nb)))
            worst = max(worst, math.degrees(math.acos(cosine)))
        assert worst < 45.0

    def test_short_input_is_returned_as_is(self) -> None:
        points = [Point(0.5, 0.5)]
        assert smooth_and_resample(points, step=0.01) == points
