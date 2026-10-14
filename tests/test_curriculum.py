"""字表元数据与主动选字测试。

本文件同时充当字表数据的**回归保护**：标注数量或分布发生变化时会立即失败，
提醒修改者确认这是有意为之，而不是手滑。
"""

from __future__ import annotations

import re
from collections import Counter
from pathlib import Path

import pytest

from handwriting.curriculum import (
    CHARACTERS,
    COLD_START_CHARS,
    COLD_START_SIZE,
    COMPLEXITY_LABELS,
    DEFAULT_CHARACTERS,
    MILESTONES,
    STRUCTURES,
    all_cells,
    cell_of,
    coverage,
    coverage_report,
    decode_cell,
    encode_cells,
    next_char,
    order_by_coverage,
    write_order,
)

# 与采集器 capture/index.html 内置的 150 字表保持一致
EXPECTED_SIZE = 150


class TestTableIntegrity:
    def test_size(self) -> None:
        assert len(CHARACTERS) == EXPECTED_SIZE
        assert len(DEFAULT_CHARACTERS) == EXPECTED_SIZE

    def test_no_duplicates(self) -> None:
        assert len(set(DEFAULT_CHARACTERS)) == EXPECTED_SIZE

    def test_every_entry_has_valid_structure_and_complexity(self) -> None:
        for char, meta in CHARACTERS.items():
            assert meta.structure in STRUCTURES, f"{char} 结构非法：{meta.structure}"
            assert meta.complexity in COMPLEXITY_LABELS, f"{char} 复杂度非法：{meta.complexity}"
            assert meta.char == char

    def test_structure_distribution(self) -> None:
        """分布断言：改动字表后若数字变化，请确认是有意调整并同步更新此处。"""
        counts = Counter(meta.structure for meta in CHARACTERS.values())
        assert counts["独体"] == 61
        assert counts["左右"] == 46
        assert counts["上下"] == 30
        assert counts["半包围"] == 11
        assert counts["包围"] == 2

    def test_complexity_distribution(self) -> None:
        counts = Counter(meta.complexity for meta in CHARACTERS.values())
        assert counts[1] == 67
        assert counts[2] == 61
        assert counts[3] == 22

    def test_cell_lookup_rejects_unknown_char(self) -> None:
        with pytest.raises(KeyError):
            cell_of("龘")

    def test_complexity_label_is_available(self) -> None:
        assert CHARACTERS["永"].complexity_label == COMPLEXITY_LABELS[1]


class TestCoverageGrid:
    def test_all_cells_are_within_known_dimensions(self) -> None:
        for structure, complexity in all_cells():
            assert structure in STRUCTURES
            assert complexity in COMPLEXITY_LABELS

    def test_every_character_maps_into_a_cell(self) -> None:
        assert len(coverage(DEFAULT_CHARACTERS)) == len(all_cells())


class TestColdStart:
    def test_size_matches_constant(self) -> None:
        assert len(COLD_START_CHARS) == COLD_START_SIZE

    def test_chars_are_in_the_table(self) -> None:
        assert set(COLD_START_CHARS) <= set(CHARACTERS)

    def test_covers_distinct_cells(self) -> None:
        """冷启动的核心要求：8 个字覆盖 8 个不同格子。

        这条断言是 ADR-0005 的实现保障 —— 只要它成立，
        用户写完 8 个字就必然拿到了 8 类不同的结构-复杂度风格样本。
        """
        cells = [cell_of(char) for char in COLD_START_CHARS]
        assert len(set(cells)) == COLD_START_SIZE

    def test_first_char_is_yong(self) -> None:
        """永字八法：一个字覆盖横竖撇捺点折钩提全部基本笔画。"""
        assert COLD_START_CHARS[0] == "永"

    def test_milestones_start_at_cold_start(self) -> None:
        assert MILESTONES[0] == COLD_START_SIZE
        assert list(MILESTONES) == sorted(MILESTONES)


class TestOrderByCoverage:
    def test_returns_full_table_without_duplicates(self) -> None:
        order = order_by_coverage()
        assert len(order) == EXPECTED_SIZE
        assert len(set(order)) == EXPECTED_SIZE
        assert set(order) == set(DEFAULT_CHARACTERS)

    def test_starts_with_curated_cold_start(self) -> None:
        order = order_by_coverage()
        assert tuple(order[:COLD_START_SIZE]) == COLD_START_CHARS

    def test_first_eleven_cover_every_cell(self) -> None:
        """冷启动 8 字 + 紧接着 3 字，应当把全部 11 个格子都覆盖到。"""
        order = order_by_coverage()
        assert {cell_of(c) for c in order[:11]} == set(all_cells())

    def test_uncovered_cells_are_filled_before_repeats(self) -> None:
        order = order_by_coverage()
        cold_cells = {cell_of(c) for c in COLD_START_CHARS}
        missing = set(all_cells()) - cold_cells
        assert {
            cell_of(c) for c in order[COLD_START_SIZE : COLD_START_SIZE + len(missing)]
        } == missing

    def test_always_picks_the_least_covered_available_cell(self) -> None:
        """轮转贪心的确切性质：每一步都取当前覆盖次数最少的**仍有余额的**格子。

        不能断言「跑完后各格子次数差不超过 1」—— 各格子容量天然不同
        （包围结构只有 2 个字），次数差必然大于 1。
        """
        order = order_by_coverage()
        remaining = Counter(cell_of(char) for char in DEFAULT_CHARACTERS)
        counts: Counter[tuple[str, int]] = Counter()

        for char in order:
            cell = cell_of(char)
            available_min = min(counts[c] for c in all_cells() if remaining[c] > 0)
            assert counts[cell] == available_min, f"{char} 所在的 {cell} 不是当前最缺的格子"
            counts[cell] += 1
            remaining[cell] -= 1

    def test_first_forty_chars_cover_every_cell_at_least_twice(self) -> None:
        """实际可用的保证：写完 40 个字，每类结构-复杂度都至少有 2 个样本。

        这是「40 字里程碑」这个数字的依据 —— 到这一步风格覆盖已经足够立体。
        """
        counts = coverage(order_by_coverage()[:40])
        for cell in all_cells():
            assert counts[cell] >= 2, f"格子 {cell} 只有 {counts[cell]} 个样本"

    def test_can_skip_curated_prefix(self) -> None:
        order = order_by_coverage(cold_start=())
        assert len(order) == EXPECTED_SIZE
        assert order[0] != COLD_START_CHARS[0] or True  # 仅要求不报错且完整

    def test_unknown_cold_start_char_is_rejected(self) -> None:
        with pytest.raises(KeyError, match="冷启动字不在字表中"):
            order_by_coverage(cold_start=("龘",))

    def test_empty_input_returns_empty(self) -> None:
        assert order_by_coverage(chars=[]) == []


class TestNextChar:
    def test_first_recommendation_is_yong(self) -> None:
        assert next_char([]) == "永"

    def test_follows_cold_start_order(self) -> None:
        collected: list[str] = []
        for expected in COLD_START_CHARS:
            assert next_char(collected) == expected
            collected.append(expected)

    def test_after_cold_start_fills_remaining_cells(self) -> None:
        collected = list(COLD_START_CHARS)
        cold_cells = {cell_of(c) for c in collected}
        expected = set(all_cells()) - cold_cells
        for _ in range(len(expected)):
            recommendation = next_char(collected)
            assert recommendation is not None
            assert cell_of(recommendation) in expected
            expected.discard(cell_of(recommendation))
            collected.append(recommendation)

    def test_never_recommends_collected_char(self) -> None:
        collected = list(DEFAULT_CHARACTERS[:60])
        assert next_char(collected) not in collected

    def test_returns_none_when_exhausted(self) -> None:
        assert next_char(list(DEFAULT_CHARACTERS)) is None

    def test_rejects_unknown_collected_char(self) -> None:
        with pytest.raises(KeyError, match="字表中没有这些字符"):
            next_char(["龘"])


class TestCoverageReport:
    def test_reports_progress(self) -> None:
        report = coverage_report(list(COLD_START_CHARS))
        assert report["collected"] == COLD_START_SIZE
        assert report["total"] == EXPECTED_SIZE

    def test_lists_missing_cells_after_cold_start(self) -> None:
        report = coverage_report(list(COLD_START_CHARS))
        missing = report["missing_cells"]
        assert isinstance(missing, list)
        assert len(missing) == len(all_cells()) - COLD_START_SIZE

    def test_no_missing_cells_when_all_collected(self) -> None:
        report = coverage_report(list(DEFAULT_CHARACTERS))
        assert report["missing_cells"] == []

    def test_milestone_advances_with_progress(self) -> None:
        assert coverage_report([])["milestone"] == 8
        assert coverage_report(list(DEFAULT_CHARACTERS[:20]))["milestone"] == 40
        assert coverage_report(list(DEFAULT_CHARACTERS[:60]))["milestone"] == 150


class TestWriteOrder:
    def test_writes_joined_string(self, tmp_path: Path) -> None:
        target = tmp_path / "order.txt"
        write_order(target)
        text = target.read_text(encoding="utf-8")
        assert len(text) == EXPECTED_SIZE
        assert text.startswith("".join(COLD_START_CHARS))
        assert len(set(text)) == EXPECTED_SIZE


class TestCellEncoding:
    def test_round_trip(self) -> None:
        for char in DEFAULT_CHARACTERS:
            code = encode_cells([char])
            assert decode_cell(code) == cell_of(char)

    def test_length_is_double(self) -> None:
        assert len(encode_cells(DEFAULT_CHARACTERS)) == EXPECTED_SIZE * 2

    def test_rejects_bad_length(self) -> None:
        with pytest.raises(ValueError, match="两字符"):
            decode_cell("1")

    def test_rejects_unknown_structure_code(self) -> None:
        with pytest.raises(ValueError, match="未知结构编码"):
            decode_cell("91")

    def test_rejects_unknown_complexity_code(self) -> None:
        with pytest.raises(ValueError, match="未知复杂度编码"):
            decode_cell("17")


class TestCollectorConsistency:
    """采集器把字表顺序与元数据内嵌为字符串常量。

    没有这道检查，改了字表却忘记重新运行 scripts/build_curriculum.py
    就会让采集器静默地按旧顺序采集 —— 而且不报任何错。
    """

    @staticmethod
    def _collector_source() -> str:
        path = Path(__file__).resolve().parents[1] / "capture" / "index.html"
        return path.read_text(encoding="utf-8")

    def test_embedded_order_matches_curriculum(self) -> None:
        source = self._collector_source()
        match = re.search(r'var ORDER = "([^"]+)";', source)
        assert match is not None, "capture/index.html 里找不到 ORDER 常量"
        assert match.group(1) == "".join(order_by_coverage()), (
            "采集器的 ORDER 与 curriculum.py 不一致，请重新运行 "
            "python scripts/build_curriculum.py 并更新 HTML"
        )

    def test_embedded_cells_match_curriculum(self) -> None:
        source = self._collector_source()
        match = re.search(r'var CELLS = "([^"]+)";', source)
        assert match is not None, "capture/index.html 里找不到 CELLS 常量"
        assert match.group(1) == encode_cells(list(order_by_coverage())), (
            "采集器的 CELLS 与 curriculum.py 不一致，请重新运行 "
            "python scripts/build_curriculum.py 并更新 HTML"
        )
