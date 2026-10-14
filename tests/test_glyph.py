"""字形骨架加载与坐标变换测试。

除标记 ``network`` 的用例外，全部使用合成数据，不依赖网络 ——
CI 与日常开发都不该因为字形站点不可用而失败。
"""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from handwriting.glyph import (
    EM_MAX_Y,
    EM_MIN_Y,
    EM_SIZE,
    Glyph,
    GlyphNotFoundError,
    HanziWriterSource,
    load_glyphs,
    parse_glyph,
)

# 与 hanzi-writer-data 的 JSON 结构一致：本模块只用 medians
SAMPLE_PAYLOAD = {
    "strokes": ["M 0 0 L 10 10 Z"],
    "medians": [[[428, 824], [503, 781]], [[300, 300]]],
    "radStrokes": [0],
}


class TestParse:
    def test_parses_medians(self) -> None:
        glyph = parse_glyph("永", SAMPLE_PAYLOAD)
        assert glyph.char == "永"
        assert glyph.stroke_count == 2
        assert glyph.point_count == 3
        assert glyph.medians[0][0] == (428.0, 824.0)

    def test_missing_medians_is_rejected(self) -> None:
        with pytest.raises(ValueError, match="缺少 medians"):
            parse_glyph("永", {"strokes": []})

    def test_empty_medians_is_rejected(self) -> None:
        with pytest.raises(ValueError, match="没有任何有效的笔画中心线"):
            parse_glyph("永", {"medians": []})

    def test_malformed_point_is_rejected(self) -> None:
        with pytest.raises(ValueError, match="第 0 笔"):
            parse_glyph("永", {"medians": [[[1, 2], ["a"]]]})

    def test_empty_stroke_is_skipped(self) -> None:
        glyph = parse_glyph("永", {"medians": [[], [[1, 2]]]})
        assert glyph.stroke_count == 1

    def test_bounds(self) -> None:
        glyph = parse_glyph("永", SAMPLE_PAYLOAD)
        assert glyph.bounds() == (300.0, 300.0, 503.0, 824.0)


class TestCoordinateTransform:
    def test_em_box_corners_map_to_unit_square(self) -> None:
        """em 框左上角 → (0, 0)，右下角 → (1, 1)。

        注意 y 轴要翻转：Make Me a Hanzi 的 y 向上，本项目的 y 向下。
        """
        glyph = Glyph("测", (((0.0, EM_MAX_Y),), ((EM_SIZE, EM_MIN_Y),)))
        result = glyph.to_stroke_set()

        top_left = result.strokes[0].points[0]
        bottom_right = result.strokes[1].points[0]
        assert (top_left.x, top_left.y) == pytest.approx((0.0, 0.0))
        assert (bottom_right.x, bottom_right.y) == pytest.approx((1.0, 1.0))

    def test_y_axis_is_flipped(self) -> None:
        # 原始数据里 y 越大越靠上，转换后 y 越小越靠上
        glyph = Glyph("测", (((0.0, 800.0),), ((0.0, 100.0),)))
        result = glyph.to_stroke_set()
        upper = result.strokes[0].points[0].y
        lower = result.strokes[1].points[0].y
        assert upper < lower

    def test_relative_size_is_preserved(self) -> None:
        """em 框固定，所以小字不会被放大到与大字一样大。

        这是不用「逐字包围盒归一化」的原因：那样「口」会被放大到
        和「国」一样大，字格的视觉比例就毁了。
        """
        small = Glyph("口", (((300.0, 500.0), (700.0, 500.0)),))
        large = Glyph("国", (((50.0, 800.0), (950.0, 100.0)),))
        assert small.to_stroke_set().total_length < large.to_stroke_set().total_length * 0.5

    def test_stroke_order_is_preserved(self) -> None:
        glyph = parse_glyph("永", SAMPLE_PAYLOAD)
        result = glyph.to_stroke_set()
        assert result.stroke_count == 2
        assert result.strokes[0].points[0].x == pytest.approx((428 - 0) / EM_SIZE)
        assert result.strokes[1].points[0].x == pytest.approx((300 - 0) / EM_SIZE)

    def test_smoothing_adds_points(self) -> None:
        glyph = parse_glyph("永", SAMPLE_PAYLOAD)
        raw = glyph.to_stroke_set(smooth=False)
        smooth = glyph.to_stroke_set(smooth=True)
        assert raw.point_count == 3
        assert smooth.point_count > raw.point_count

    def test_single_point_stroke_survives(self) -> None:
        glyph = Glyph("测", (((500.0, 500.0),),))
        result = glyph.to_stroke_set()
        assert result.stroke_count == 1
        assert result.point_count == 1


class TestHanziWriterSource:
    def test_cache_path_uses_codepoint(self, tmp_path: Path) -> None:
        """缓存文件名用码位而非字符本身：Windows 上非 ASCII 文件名容易踩编码坑。"""
        source = HanziWriterSource(cache_dir=tmp_path)
        assert source.cache_path("永").name == "U+6C38.json"

    def test_loads_from_cache_without_network(self, tmp_path: Path) -> None:
        (tmp_path / "U+6C38.json").write_text(
            json.dumps(SAMPLE_PAYLOAD, ensure_ascii=False), encoding="utf-8"
        )
        source = HanziWriterSource(cache_dir=tmp_path, offline=True)
        glyph = source.load("永")
        assert glyph.stroke_count == 2

    def test_offline_without_cache_raises(self, tmp_path: Path) -> None:
        source = HanziWriterSource(cache_dir=tmp_path, offline=True)
        with pytest.raises(GlyphNotFoundError, match="离线模式"):
            source.load("永")

    def test_rejects_multi_character_input(self, tmp_path: Path) -> None:
        source = HanziWriterSource(cache_dir=tmp_path, offline=True)
        with pytest.raises(ValueError, match="一次只能加载一个字符"):
            source.load("永西")

    def test_preload_reports_failures_without_raising(self, tmp_path: Path) -> None:
        (tmp_path / "U+6C38.json").write_text(json.dumps(SAMPLE_PAYLOAD), encoding="utf-8")
        source = HanziWriterSource(cache_dir=tmp_path, offline=True)
        failures = source.preload(["永", "西"])
        assert "永" not in failures
        assert "西" in failures

    def test_base_url_without_trailing_slash_is_normalised(self, tmp_path: Path) -> None:
        source = HanziWriterSource(cache_dir=tmp_path, base_url="https://example.com/data")
        assert source.base_url == "https://example.com/data/"

    def test_multiple_mirrors_are_kept_in_order(self, tmp_path: Path) -> None:
        source = HanziWriterSource(
            cache_dir=tmp_path, base_urls=["https://a.example", "https://b.example/"]
        )
        assert source.base_urls == ("https://a.example/", "https://b.example/")
        assert source.base_url == "https://a.example/"

    def test_default_has_a_fallback_mirror(self, tmp_path: Path) -> None:
        """单源不可靠：实测 jsdelivr 会偶发断连，必须有备用镜像。"""
        source = HanziWriterSource(cache_dir=tmp_path)
        assert len(source.base_urls) >= 2
        assert len(set(source.base_urls)) == len(source.base_urls)

    def test_empty_mirror_list_is_rejected(self, tmp_path: Path) -> None:
        with pytest.raises(ValueError, match="至少需要一个字形数据源地址"):
            HanziWriterSource(cache_dir=tmp_path, base_urls=[])

    def test_invalid_retries_is_rejected(self, tmp_path: Path) -> None:
        with pytest.raises(ValueError, match="retries"):
            HanziWriterSource(cache_dir=tmp_path, retries=0)


class TestLoadGlyphs:
    def test_partial_failure_does_not_abort(self, tmp_path: Path) -> None:
        (tmp_path / "U+6C38.json").write_text(json.dumps(SAMPLE_PAYLOAD), encoding="utf-8")
        source = HanziWriterSource(cache_dir=tmp_path, offline=True)

        loaded, failures = load_glyphs(["永", "西"], source)
        assert set(loaded) == {"永"}
        assert set(failures) == {"西"}

    def test_deduplicates_work(self, tmp_path: Path) -> None:
        (tmp_path / "U+6C38.json").write_text(json.dumps(SAMPLE_PAYLOAD), encoding="utf-8")
        source = HanziWriterSource(cache_dir=tmp_path, offline=True)
        loaded, failures = load_glyphs(["永", "永", "永"], source)
        assert set(loaded) == {"永"}
        assert failures == {}


@pytest.mark.network
class TestRealGlyphData:
    """需要联网。用 ``pytest --run-network`` 启用。"""

    def test_downloads_and_caches(self, tmp_path: Path) -> None:
        source = HanziWriterSource(cache_dir=tmp_path)
        glyph = source.load("永")

        assert glyph.char == "永"
        assert glyph.stroke_count == 5  # 永字八法虽名八法，实际五笔
        assert source.cache_path("永").is_file()

    def test_second_load_uses_cache(self, tmp_path: Path) -> None:
        source = HanziWriterSource(cache_dir=tmp_path)
        source.load("永")
        offline = HanziWriterSource(cache_dir=tmp_path, offline=True)
        assert offline.load("永").stroke_count == 5

    def test_glyph_sits_inside_the_em_box(self, tmp_path: Path) -> None:
        """真实字形的归一化坐标应大体落在 0~1 内。

        超出说明 em 框标定错了，字会被裁掉或缩得过小。
        """
        source = HanziWriterSource(cache_dir=tmp_path)
        for char in "永国口一人小":
            stroke_set = source.load(char).to_stroke_set()
            box = stroke_set.bbox()
            assert box is not None
            assert box[0] >= -0.1 and box[2] <= 1.1, f"{char} 横向越界：{box}"
            assert box[1] >= -0.1 and box[3] <= 1.1, f"{char} 纵向越界：{box}"

    def test_small_characters_stay_small(self, tmp_path: Path) -> None:
        """「口」应当明显小于「国」—— 用真实数据验证 em 框标定。

        若误用逐字包围盒归一化，两者会一样大。
        """
        source = HanziWriterSource(cache_dir=tmp_path)
        kou = source.load("口").to_stroke_set().bbox()
        guo = source.load("国").to_stroke_set().bbox()
        assert kou is not None and guo is not None

        kou_area = (kou[2] - kou[0]) * (kou[3] - kou[1])
        guo_area = (guo[2] - guo[0]) * (guo[3] - guo[1])
        assert kou_area < guo_area * 0.6

    def test_horizontal_stroke_sits_near_the_middle(self, tmp_path: Path) -> None:
        """「一」应当是一条位于字格中偏上的横线，而不是撑满整格的方块。"""
        source = HanziWriterSource(cache_dir=tmp_path)
        box = source.load("一").to_stroke_set().bbox()
        assert box is not None
        assert box[2] - box[0] > 0.5  # 够宽
        assert box[3] - box[1] < 0.15  # 但很扁
        assert 0.3 < (box[1] + box[3]) / 2 < 0.7  # 位于中部
