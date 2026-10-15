"""版面测试。

与 C++ 侧 core/tests/test_layout.cpp 对应。最后的 TestCrossImplementation
会实际调用 C++ 工具比对两份实现，防止漂移（见 ADR-0003 的后果一节）。
"""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

import pytest

from handwriting.glyph import BASELINE_RATIO
from handwriting.layout import (
    NOTEBOOK_HEIGHT_MM,
    NOTEBOOK_LINES,
    NOTEBOOK_WIDTH_MM,
    LayoutSettings,
    Mode,
    compute,
    compute_uniform,
    resolve_char_size,
    resolve_chars_per_line,
)

REPO_ROOT = Path(__file__).resolve().parents[1]
NOTEBOOK_PITCH = NOTEBOOK_HEIGHT_MM / NOTEBOOK_LINES


def notebook(**kwargs: object) -> LayoutSettings:
    return LayoutSettings(
        mode=Mode.NOTEBOOK,
        page_width_mm=NOTEBOOK_WIDTH_MM,
        page_height_mm=NOTEBOOK_HEIGHT_MM,
        line_count=NOTEBOOK_LINES,
        **kwargs,  # type: ignore[arg-type]
    )


class TestNotebookDefaults:
    def test_default_settings_are_a_common_exercise_book(self) -> None:
        settings = LayoutSettings()
        assert settings.mode is Mode.NOTEBOOK
        assert settings.page_width_mm == NOTEBOOK_WIDTH_MM
        assert settings.page_height_mm == NOTEBOOK_HEIGHT_MM
        assert settings.line_count == NOTEBOOK_LINES

    def test_auto_char_size_equals_line_pitch(self) -> None:
        assert resolve_char_size(notebook()) == pytest.approx(NOTEBOOK_PITCH)

    def test_auto_chars_per_line(self) -> None:
        # 150 / 9.68 = 15.5 → 15
        assert resolve_chars_per_line(notebook()) == 15

    def test_page_dimensions(self) -> None:
        page = compute([30], notebook())
        assert page.width_mm == pytest.approx(150.0)
        assert page.height_mm == pytest.approx(213.0)
        assert page.chars_per_line == 15
        assert len(page.cells) == 30
        assert len(page.rule_lines_mm) == 22

    def test_rule_positions(self) -> None:
        page = compute([1], notebook())
        for index, y in enumerate(page.rule_lines_mm):
            assert y == pytest.approx((index + 1) * NOTEBOOK_PITCH)
        assert page.rule_lines_mm[-1] == pytest.approx(213.0)


class TestBaselineOnRule:
    """横条本的核心性质：字压在横线上。"""

    def test_every_baseline_lands_on_a_rule(self) -> None:
        page = compute([30], notebook())
        for cell in page.cells:
            assert cell.baseline_mm == pytest.approx((cell.line + 1) * NOTEBOOK_PITCH)

    def test_cell_top_is_derived_from_baseline(self) -> None:
        page = compute([1], notebook())
        cell = page.cells[0]
        assert cell.y_mm == pytest.approx(cell.baseline_mm - BASELINE_RATIO * cell.size_mm)
        assert cell.y_mm < cell.baseline_mm

    def test_cells_stay_inside_the_page(self) -> None:
        page = compute([30], notebook())
        for cell in page.cells:
            assert cell.y_mm >= 0.0
            assert cell.baseline_mm <= 213.0 + 1e-9
            assert cell.x_mm >= 0.0
            assert cell.x_mm + cell.size_mm <= 150.0 + 1e-9


class TestArrangement:
    def test_rows_and_columns(self) -> None:
        page = compute([30], notebook())
        assert (page.cells[0].line, page.cells[0].column) == (0, 0)
        assert (page.cells[14].line, page.cells[14].column) == (0, 14)
        assert (page.cells[15].line, page.cells[15].column) == (1, 0)
        assert (page.cells[29].line, page.cells[29].column) == (1, 14)

    def test_x_increases_within_a_row(self) -> None:
        page = compute([15], notebook())
        for index in range(1, len(page.cells)):
            assert page.cells[index].x_mm > page.cells[index - 1].x_mm

    def test_first_line_offset(self) -> None:
        page = compute([1], notebook(first_line=3))
        assert page.cells[0].line == 2
        assert page.cells[0].baseline_mm == pytest.approx(3 * NOTEBOOK_PITCH)

    def test_explicit_newline_starts_a_new_line(self) -> None:
        page = compute([3, 3], notebook())
        assert len(page.cells) == 6
        assert page.cells[0].line == 0
        assert page.cells[2].line == 0
        assert page.cells[3].line == 1
        assert page.cells[5].line == 1

    def test_long_row_wraps(self) -> None:
        page = compute([20], notebook())
        assert page.cells[14].line == 0
        assert page.cells[15].line == 1


class TestExplicitSizes:
    def test_manual_char_size_and_chars_per_line(self) -> None:
        settings = notebook(char_size_mm=8.0, chars_per_line=18)
        assert resolve_char_size(settings) == pytest.approx(8.0)
        assert resolve_chars_per_line(settings) == 18

        page = compute([18], settings)
        assert page.cells[0].size_mm == pytest.approx(8.0)
        # 列宽 150/18 = 8.33，字宽 8 → 水平居中
        assert page.cells[0].x_mm == pytest.approx((150.0 / 18.0 - 8.0) / 2.0)
        assert page.cells[-1].line == 0


class TestEmptyAndInvalid:
    def test_empty_input(self) -> None:
        page = compute([0], notebook())
        assert page.cells == []
        assert len(page.rule_lines_mm) == 22

    def test_compute_uniform_splits_into_rows(self) -> None:
        page = compute_uniform(30, notebook())
        assert len(page.cells) == 30
        assert page.cells[15].line == 1

    def test_zero_line_count_is_rejected(self) -> None:
        settings = notebook()
        settings.line_count = 0
        with pytest.raises(ValueError, match="line_count"):
            compute([1], settings)


class TestGridMode:
    def test_basic_geometry(self) -> None:
        settings = LayoutSettings(
            mode=Mode.GRID,
            char_size_mm=20.0,
            char_gap_mm=3.0,
            line_gap_mm=10.0,
            margin_x_mm=10.0,
            margin_y_mm=10.0,
            grid_chars_per_line=10,
        )
        page = compute([20], settings)
        assert page.chars_per_line == 10
        assert page.rule_lines_mm == []
        assert page.cells[0].x_mm == pytest.approx(10.0)
        assert page.cells[0].y_mm == pytest.approx(10.0)
        assert page.cells[1].x_mm == pytest.approx(33.0)
        assert page.cells[10].y_mm == pytest.approx(40.0)
        assert page.cells[0].baseline_mm == pytest.approx(10.0 + BASELINE_RATIO * 20.0)
        assert page.width_mm == pytest.approx(10.0 * 2 + 10 * 20.0 + 9 * 3.0)
        assert page.height_mm == pytest.approx(10.0 * 2 + 2 * 20.0 + 1 * 10.0)


def _find_layout_dump() -> Path | None:
    for name in ("hr-layout-dump.exe", "hr-layout-dump"):
        for build in ("build", "build-qt"):
            candidate = REPO_ROOT / build / "core" / name
            if candidate.is_file():
                return candidate
    found = shutil.which("hr-layout-dump")
    return Path(found) if found else None


class TestCrossImplementation:
    """调用 C++ 工具比对两份版面实现。

    C++ 与 Python 各有一份版面实现，漂移会让 Qt 预览与命令行导出的
    G 代码不一致 —— 这类偏差肉眼几乎看不出来。所以必须真的去比。
    """

    CASES: tuple = (
        # mode, width, height, lines, first_line, char_size, per_line, rows
        ("notebook", 150.0, 213.0, 22, 1, 0.0, 0, [30]),
        ("notebook", 150.0, 213.0, 22, 3, 0.0, 0, [5, 5, 5]),
        ("notebook", 150.0, 213.0, 22, 1, 8.0, 18, [40]),
        ("notebook", 120.0, 180.0, 18, 2, 0.0, 12, [12, 12]),
        ("grid", 150.0, 213.0, 22, 1, 20.0, 10, [25]),
        ("grid", 150.0, 213.0, 22, 1, 15.0, 0, [7]),
    )

    @pytest.mark.parametrize("case", CASES)
    def test_layout_matches(self, case: tuple) -> None:
        binary = _find_layout_dump()
        if binary is None:
            pytest.skip("未找到 hr-layout-dump，先构建 C++ 侧（cmake --build build）")

        mode, width, height, lines, first_line, char_size, per_line, rows = case
        arguments = [
            str(binary),
            mode,
            str(width),
            str(height),
            str(lines),
            str(first_line),
            str(char_size),
            str(per_line),
            *[str(row) for row in rows],
        ]
        completed = subprocess.run(arguments, capture_output=True, text=True, check=True)
        lines_out = [line for line in completed.stdout.splitlines() if line]

        settings = LayoutSettings(
            mode=Mode.NOTEBOOK if mode == "notebook" else Mode.GRID,
            page_width_mm=width,
            page_height_mm=height,
            line_count=lines,
            first_line=first_line,
            char_size_mm=char_size,
            chars_per_line=per_line,
        )
        page = compute(rows, settings)

        cell_lines = [line for line in lines_out if not line.startswith("PAGE")]
        assert len(cell_lines) == len(page.cells), "cell 数量不一致"

        for text, cell in zip(cell_lines, page.cells, strict=True):
            x, y, size, baseline, line, column = text.split("\t")
            assert float(x) == pytest.approx(cell.x_mm, abs=1e-6)
            assert float(y) == pytest.approx(cell.y_mm, abs=1e-6)
            assert float(size) == pytest.approx(cell.size_mm, abs=1e-6)
            assert float(baseline) == pytest.approx(cell.baseline_mm, abs=1e-6)
            assert int(line) == cell.line
            assert int(column) == cell.column

        page_line = next(line for line in lines_out if line.startswith("PAGE"))
        _, width_mm, height_mm, per_line_out = page_line.split("\t")
        assert float(width_mm) == pytest.approx(page.width_mm, abs=1e-6)
        assert float(height_mm) == pytest.approx(page.height_mm, abs=1e-6)
        assert int(per_line_out) == page.chars_per_line
