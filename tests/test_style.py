"""风格特征提取测试。

每个测试都针对一个**具体可验证的几何事实**，而不是「跑起来没报错」。
例如倾斜角测试用的是一条已知倾角的直线，折角测试用的是标准直角。
"""

from __future__ import annotations

import math
from dataclasses import FrozenInstanceError
from itertools import pairwise

import pytest

from handwriting.stroke import Point, Stroke, StrokeSet
from handwriting.style import (
    CORNER_THRESHOLD_DEG,
    StyleFeatures,
    distance,
    extract,
    mean_features,
    resample,
)


def line(x0: float, y0: float, x1: float, y1: float, t0: int = 0, t1: int = 100) -> Stroke:
    return Stroke([Point(x0, y0, 0.5, t0), Point(x1, y1, 0.5, t1)])


class TestResample:
    def test_emits_points_at_fixed_arc_length(self) -> None:
        result = resample(line(0.0, 0.0, 1.0, 0.0), step=0.1)
        assert 10 <= len(result) <= 12
        assert result.points[0].x == pytest.approx(0.0)
        assert result.points[-1].x == pytest.approx(1.0)

    def test_spacing_is_uniform(self) -> None:
        result = resample(line(0.0, 0.0, 1.0, 0.0), step=0.05)
        gaps = [b.x - a.x for a, b in pairwise(result.points)]
        interior = gaps[:-1]  # 末段是补齐的残段，不参与
        for gap in interior:
            assert gap == pytest.approx(0.05, abs=1e-9)

    def test_stroke_shorter_than_step_is_kept(self) -> None:
        result = resample(line(0.0, 0.0, 0.01, 0.0), step=0.1)
        assert len(result) == 2

    def test_single_point_stroke_is_untouched(self) -> None:
        stroke = Stroke([Point(0.5, 0.5)])
        assert resample(stroke) is stroke

    def test_rejects_non_positive_step(self) -> None:
        with pytest.raises(ValueError, match="step 必须为正"):
            resample(line(0.0, 0.0, 1.0, 0.0), step=0.0)

    def test_pressure_is_interpolated(self) -> None:
        stroke = Stroke([Point(0.0, 0.0, 0.0, 0), Point(1.0, 0.0, 1.0, 100)])
        result = resample(stroke, step=0.1)
        assert result.points[0].pressure == pytest.approx(0.0)
        assert result.points[-1].pressure == pytest.approx(1.0)


class TestSlant:
    def test_vertical_stroke_has_zero_slant(self) -> None:
        features = extract(StrokeSet([line(0.5, 0.0, 0.5, 1.0)]))
        assert features.slant_deg == pytest.approx(0.0, abs=0.5)

    def test_right_leaning_stroke_is_positive(self) -> None:
        # dx=0.1, dy=1.0 => atan2(0.1, 1.0) = 5.71°
        features = extract(StrokeSet([line(0.5, 0.0, 0.6, 1.0)]))
        assert features.slant_deg == pytest.approx(math.degrees(math.atan2(0.1, 1.0)), abs=0.5)
        assert features.slant_deg > 0

    def test_left_leaning_stroke_is_negative(self) -> None:
        features = extract(StrokeSet([line(0.6, 0.0, 0.5, 1.0)]))
        assert features.slant_deg < 0

    def test_horizontal_only_glyph_has_zero_slant(self) -> None:
        # 全是横画时没有偏竖直的段，倾斜角应回退为 0 而不是报错
        features = extract(StrokeSet([line(0.0, 0.5, 1.0, 0.5)]))
        assert features.slant_deg == pytest.approx(0.0)


class TestCorners:
    def test_straight_stroke_has_no_corner(self) -> None:
        features = extract(StrokeSet([line(0.0, 0.5, 1.0, 0.5)]))
        assert features.corner_deg == pytest.approx(0.0)
        assert features.corner_rate == pytest.approx(0.0)
        assert features.turn_mean_deg == pytest.approx(0.0, abs=0.01)

    def test_right_angle_is_detected(self) -> None:
        # 口 的右上角：横走到 (0.5, 0.5) 后向下折
        corner = Stroke([Point(0.0, 0.5), Point(0.5, 0.5), Point(0.5, 1.0)])
        features = extract(StrokeSet([corner]))
        assert features.corner_deg == pytest.approx(90.0, abs=1.0)
        assert features.corner_rate > 0.0

    def test_corner_threshold_is_respected(self) -> None:
        # 构造一个恰好低于阈值的转折：第二段相对第一段的延长线只偏 25°。
        # 注意「转折角」是两段方向向量之间的夹角，不是第二段与竖直方向的夹角 ——
        # 若按后者构造，得到的是 90−25=65°，测试就测错了东西。
        rad = math.radians(CORNER_THRESHOLD_DEG - 5.0)
        corner = Stroke(
            [
                Point(0.0, 0.5),
                Point(0.5, 0.5),
                Point(0.5 + 0.5 * math.cos(rad), 0.5 + 0.5 * math.sin(rad)),
            ]
        )
        features = extract(StrokeSet([corner]))
        assert features.turn_mean_deg > 0.0  # 转折确实存在
        assert features.corner_deg == pytest.approx(0.0)  # 但没到「折」的程度

    def test_smooth_curve_has_small_mean_turn(self) -> None:
        # 近似圆弧：整体有弯曲但不应被判为折
        points = [
            Point(0.5 + 0.3 * math.sin(i * 0.1), 0.5 - 0.3 * math.cos(i * 0.1), 0.5, i * 10)
            for i in range(20)
        ]
        features = extract(StrokeSet([Stroke(points)]))
        assert features.turn_mean_deg > 0.0
        assert features.corner_deg == pytest.approx(0.0)


class TestGeometry:
    def test_aspect_ratio(self) -> None:
        wide = StrokeSet([line(0.0, 0.5, 1.0, 0.5), line(0.0, 0.0, 0.0, 0.5)])
        features = extract(wide)
        assert features.aspect_ratio == pytest.approx(2.0)

    def test_straightness_is_one_for_pure_lines(self) -> None:
        features = extract(StrokeSet([line(0.0, 0.0, 1.0, 0.0)]))
        assert features.straightness == pytest.approx(1.0)

    def test_straightness_drops_for_detour(self) -> None:
        # 绕远路回到同一点附近，直线度应显著小于 1
        detour = Stroke(
            [Point(0.0, 0.0), Point(0.5, 0.0), Point(0.5, 0.5), Point(0.0, 0.5), Point(0.0, 0.01)]
        )
        features = extract(StrokeSet([detour]))
        assert features.straightness < 0.5

    def test_ink_density_measures_compactness_not_size(self) -> None:
        # 同一个包围盒内，笔画越多越长，墨迹密度越高
        sparse = StrokeSet([line(0.0, 0.0, 0.5, 0.0), line(0.0, 0.0, 0.0, 0.5)])
        dense = StrokeSet(
            [
                line(0.0, 0.0, 0.5, 0.0),
                line(0.0, 0.0, 0.0, 0.5),
                line(0.5, 0.0, 0.5, 0.5),
                line(0.0, 0.5, 0.5, 0.5),
            ]
        )
        assert extract(dense).ink_density > extract(sparse).ink_density

    def test_ink_density_is_scale_invariant(self) -> None:
        """墨迹密度是形状特征，不是尺寸特征 —— 大字小字应当得到同一个值。

        这正是它有用的原因：用户可以写得大或小，但字写得紧凑还是舒展是风格。
        """
        small = StrokeSet([line(0.0, 0.0, 0.1, 0.0), line(0.0, 0.0, 0.0, 0.1)])
        large = StrokeSet([line(0.0, 0.0, 1.0, 0.0), line(0.0, 0.0, 0.0, 1.0)])
        assert extract(small).ink_density == pytest.approx(extract(large).ink_density)

    def test_stroke_length_cv_is_zero_for_equal_strokes(self) -> None:
        equal = StrokeSet([line(0.0, 0.0, 0.5, 0.0), line(0.0, 0.5, 0.5, 0.5)])
        assert extract(equal).stroke_length_cv == pytest.approx(0.0)

    def test_stroke_count_is_reported(self) -> None:
        three = StrokeSet(
            [line(0.0, 0.0, 0.1, 0.0), line(0.0, 0.2, 0.1, 0.2), line(0.0, 0.4, 0.1, 0.4)]
        )
        assert extract(three).stroke_count == 3

    def test_empty_glyph_is_rejected(self) -> None:
        with pytest.raises(ValueError, match="空字形"):
            extract(StrokeSet([]))


class TestPressure:
    def test_constant_pressure_has_zero_std(self) -> None:
        stroke = Stroke([Point(i / 10, 0.0, 0.6, i * 10) for i in range(11)])
        features = extract(StrokeSet([stroke]))
        assert features.pressure_mean == pytest.approx(0.6)
        assert features.pressure_std == pytest.approx(0.0)

    def test_taper_is_positive_when_middle_is_heavier(self) -> None:
        # 起收笔轻、中段重 —— 典型的提按
        pressures = [0.2, 0.3, 0.8, 0.9, 0.9, 0.8, 0.3, 0.2]
        stroke = Stroke([Point(i / 10, 0.0, p, i * 10) for i, p in enumerate(pressures)])
        features = extract(StrokeSet([stroke]))
        assert features.pressure_taper is not None
        assert features.pressure_taper > 0.3

    def test_taper_is_negative_when_middle_is_lighter(self) -> None:
        pressures = [0.9, 0.8, 0.3, 0.2, 0.2, 0.3, 0.8, 0.9]
        stroke = Stroke([Point(i / 10, 0.0, p, i * 10) for i, p in enumerate(pressures)])
        assert extract(StrokeSet([stroke])).pressure_taper < -0.3

    def test_taper_is_none_for_short_stroke(self) -> None:
        assert extract(StrokeSet([line(0.0, 0.0, 1.0, 0.0)])).pressure_taper is None


class TestRhythm:
    def test_speed_is_measured(self) -> None:
        # 0.5 归一化单位 / 100 ms = 5 单位/秒
        features = extract(StrokeSet([line(0.0, 0.0, 0.5, 0.0, t0=0, t1=100)]))
        assert features.speed_mean == pytest.approx(5.0)
        assert features.speed_std == pytest.approx(0.0)

    def test_pause_ratio_is_none_without_start_times(self) -> None:
        assert extract(StrokeSet([line(0.0, 0.0, 0.5, 0.0)])).pause_ratio is None

    def test_pause_ratio_reflects_gap_between_strokes(self) -> None:
        # 第一笔 0~100 ms，第二笔 400~500 ms => 总时长 500，书写 200，抬笔 300
        first = Stroke([Point(0.0, 0.0, 0.5, 0), Point(0.5, 0.0, 0.5, 100)])
        second = Stroke([Point(0.0, 0.5, 0.5, 0), Point(0.5, 0.5, 0.5, 100)])
        features = extract(StrokeSet([first, second]), start_times=[0, 400])
        assert features.pause_ratio == pytest.approx(0.6)

    def test_pause_ratio_is_none_when_length_mismatched(self) -> None:
        first = Stroke([Point(0.0, 0.0, 0.5, 0), Point(0.5, 0.0, 0.5, 100)])
        assert extract(StrokeSet([first]), start_times=[0, 100]).pause_ratio is None


class TestVectorAndDistance:
    def test_vector_length_matches_field_count(self) -> None:
        features = extract(StrokeSet([line(0.0, 0.0, 1.0, 0.0)]))
        assert len(features.to_vector()) == len(features.numeric_fields)
        assert len(features.availability()) == len(features.numeric_fields)

    def test_availability_marks_missing_features(self) -> None:
        features = extract(StrokeSet([line(0.0, 0.0, 1.0, 0.0)]))
        mask = dict(zip(features.numeric_fields, features.availability(), strict=False))
        assert mask["pause_ratio"] is False
        assert mask["slant_deg"] is True

    def test_distance_to_self_is_zero(self) -> None:
        features = extract(StrokeSet([line(0.0, 0.0, 1.0, 0.0)]))
        assert distance(features, features) == pytest.approx(0.0)

    def test_distance_grows_with_difference(self) -> None:
        upright = extract(StrokeSet([line(0.5, 0.0, 0.5, 1.0)]))
        leaning = extract(StrokeSet([line(0.5, 0.0, 0.8, 1.0)]))
        assert distance(upright, leaning) > 0.0

    def test_distance_ignores_features_missing_on_either_side(self) -> None:
        # 一个带 pause_ratio、一个不带，距离仍应可计算
        first = Stroke([Point(0.0, 0.0, 0.5, 0), Point(0.5, 0.0, 0.5, 100)])
        second = Stroke([Point(0.0, 0.5, 0.5, 0), Point(0.5, 0.5, 0.5, 100)])
        with_pause = extract(StrokeSet([first, second]), start_times=[0, 400])
        without = extract(StrokeSet([first, second]))
        assert with_pause.pause_ratio is not None
        assert without.pause_ratio is None
        assert distance(with_pause, without) >= 0.0


class TestMeanFeatures:
    def test_average_of_identical_samples_is_itself(self) -> None:
        features = extract(StrokeSet([line(0.5, 0.0, 0.5, 1.0)]))
        averaged = mean_features([features, features])
        assert averaged.slant_deg == pytest.approx(features.slant_deg)
        assert averaged.stroke_count == features.stroke_count

    def test_missing_feature_propagates_to_none(self) -> None:
        first = Stroke([Point(0.0, 0.0, 0.5, 0), Point(0.5, 0.0, 0.5, 100)])
        second = Stroke([Point(0.0, 0.5, 0.5, 0), Point(0.5, 0.5, 0.5, 100)])
        with_pause = extract(StrokeSet([first, second]), start_times=[0, 400])
        without = extract(StrokeSet([first, second]))
        assert mean_features([with_pause, without]).pause_ratio is None

    def test_empty_input_is_rejected(self) -> None:
        with pytest.raises(ValueError, match="至少需要一个样本"):
            mean_features([])


def test_style_features_is_frozen() -> None:
    features = extract(StrokeSet([line(0.0, 0.0, 1.0, 0.0)]))
    assert isinstance(features, StyleFeatures)
    with pytest.raises(FrozenInstanceError):
        features.slant_deg = 1.0  # type: ignore[misc]
