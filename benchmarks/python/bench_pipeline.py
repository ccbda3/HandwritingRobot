"""性能基准：与 benchmarks/cpp/bench_pipeline.cpp 使用完全相同的合成负载。

用法：
    python benchmarks/python/bench_pipeline.py

用途是给出 Python 侧的基线数字，与 C++ 侧对比后再决定是否移植某段逻辑。
判定标准是「移植能省下的时间占端到端总耗时的比例」，而不是单看某段代码快了多少倍。
"""

from __future__ import annotations

import random
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "src"))

from handwriting.gcode import PlotSettings, strokes_to_gcode_with_stats
from handwriting.stroke import Point, Stroke, StrokeSet

CHAR_COUNT = 100
STROKES_PER_CHAR = 8
POINTS_PER_STROKE = 60
GRBL_LINES_PER_SECOND = 380.0  # GRBL @ 115200 baud 实测吞吐
SEED = 20261014


def make_page() -> list[list[StrokeSet]]:
    """合成一页字迹。

    用随机游走而不是独立随机点：真实笔画的相邻采样点是连续的，
    独立随机点会让路径长度虚高一个数量级，从而把「机床运行时间」算错。
    """
    rng = random.Random(SEED)
    rows: list[list[StrokeSet]] = []
    for _ in range(CHAR_COUNT):
        strokes: list[Stroke] = []
        for _ in range(STROKES_PER_CHAR):
            x = rng.uniform(0.05, 0.95)
            y = rng.uniform(0.05, 0.95)
            points: list[Point] = []
            for i in range(POINTS_PER_STROKE):
                points.append(Point(x, y, rng.random(), i * 8))
                x = min(0.95, max(0.05, x + rng.uniform(-0.010, 0.010)))
                y = min(0.95, max(0.05, y + rng.uniform(-0.010, 0.010)))
            strokes.append(Stroke(points))
        rows.append([StrokeSet(strokes, "永")])
    return rows


def main() -> int:
    rows = make_page()
    total_points = sum(g.point_count for row in rows for g in row)

    start = time.perf_counter()
    for row in rows:
        for glyph in row:
            glyph.fit_to_box(1.0)
    norm_ms = (time.perf_counter() - start) * 1000.0

    start = time.perf_counter()
    result = strokes_to_gcode_with_stats(rows, PlotSettings())
    gcode_ms = (time.perf_counter() - start) * 1000.0

    stats = result.stats
    line_count = stats.line_count
    serial_seconds = line_count / GRBL_LINES_PER_SECOND
    total_seconds = serial_seconds + stats.estimated_seconds
    compute_seconds = (norm_ms + gcode_ms) / 1000.0

    print("\n=== HandwritingRobot Python 基准 ===")
    print(
        f"负载           : {CHAR_COUNT} 字 x {STROKES_PER_CHAR} 笔 x "
        f"{POINTS_PER_STROKE} 点 = {total_points} 点"
    )
    print(f"归一化耗时     : {norm_ms:8.2f} ms  ({total_points / norm_ms / 1000.0:.1f} M点/秒)")
    print(
        f"G 代码生成耗时 : {gcode_ms:8.2f} ms  ({line_count} 行, "
        f"{line_count / gcode_ms / 1000.0:.2f} M行/秒)"
    )
    print(f"计算合计       : {norm_ms + gcode_ms:8.2f} ms")
    print("-" * 43)
    print(f"绘制路径长度   : {stats.draw_length_mm:8.1f} mm")
    print(f"空行程长度     : {stats.travel_length_mm:8.1f} mm")
    print(f"机床预计运行   : {stats.estimated_seconds:8.1f} s  (仅运动时间)")
    print(
        f"串口传输下限   : {serial_seconds:8.1f} s  ({line_count} 行 @ "
        f"{GRBL_LINES_PER_SECOND:.0f} 行/秒)"
    )
    print("-" * 43)
    print(f"计算占端到端   : {100.0 * compute_seconds / total_seconds:8.3f} %")
    print("=> 瓶颈在串口与机械运动，不在计算语言\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
