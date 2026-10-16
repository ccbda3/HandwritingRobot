"""整行笔迹的「字」分段。

整行采集时字与字之间没有显式边界，需要在事后切分。因为是**在线**书写
（带时序），可用信号按可靠性由低到高：

1. 抬笔时间间隔 —— 同一字内笔画间隔短或为 0，字与字之间通常有明显停顿
2. 空间间隙   —— 字间水平空隙一般大于字内（本模块暂未使用）
3. 已知文本   —— 字数已知时，分段变成受约束的对齐，可靠性最高

本模块实现 1 + 3：先用抬笔间隔粗分，再用「字数」约束做合并 / 拆分修正。
零依赖、纯时序计算，不依赖任何参考字形。后续可在此基础上接入基于标准字形的
DTW 精修（见 ``docs/data-format.md`` 的整行格式一节）。
"""

from __future__ import annotations

from collections.abc import Sequence

from handwriting.stroke import Stroke

DEFAULT_GAP_MS = 150.0
"""抬笔间隔超过该值即视为字与字之间的停顿。经验值，随设备与书写速度调整。"""


def stroke_gaps(strokes: Sequence[Stroke], stroke_start_t: Sequence[int]) -> list[float]:
    """相邻笔画之间的抬笔间隔（毫秒）。

    ``gap[i]`` = 第 i+1 笔起笔时刻 − 第 i 笔收笔时刻，重叠或时钟回退时夹到 0。
    返回长度恒为 ``len(strokes) - 1``。
    """
    if len(strokes) != len(stroke_start_t):
        raise ValueError(
            f"strokes 与 stroke_start_t 长度不一致：{len(strokes)} vs {len(stroke_start_t)}"
        )

    gaps: list[float] = []
    for i in range(len(strokes) - 1):
        previous_end = stroke_start_t[i] + strokes[i].duration_ms
        gaps.append(max(0.0, float(stroke_start_t[i + 1] - previous_end)))
    return gaps


def segment_by_gap(
    strokes: Sequence[Stroke],
    stroke_start_t: Sequence[int],
    gap_ms: float = DEFAULT_GAP_MS,
) -> list[tuple[int, int]]:
    """只按抬笔间隔切分，返回 ``[start, end)`` 笔画区间列表。"""
    if len(strokes) != len(stroke_start_t):
        raise ValueError("strokes 与 stroke_start_t 长度不一致")
    if not strokes:
        return []

    gaps = stroke_gaps(strokes, stroke_start_t)
    ranges: list[tuple[int, int]] = []
    start = 0
    for i, gap in enumerate(gaps, start=1):
        if gap > gap_ms:
            ranges.append((start, i))
            start = i
    ranges.append((start, len(strokes)))
    return ranges


def segment_line(
    strokes: Sequence[Stroke],
    stroke_start_t: Sequence[int],
    text: str,
    gap_ms: float = DEFAULT_GAP_MS,
) -> list[tuple[int, int]]:
    """在「字数 = ``len(text)``」约束下分段，返回逐字对应的笔画区间。

    流程：抬笔间隔粗分 → 段数多于字数则合并间隔最小的相邻段 →
    段数少于字数则拆分内部间隔最大的段。

    每个字至少分到一笔，因此 ``len(text)`` 不能大于笔画总数；
    超出时抛 :class:`ValueError`（这种情况说明采集本身有问题，不应静默凑数）。
    """
    target = len(text)
    if target == 0:
        return []
    if not strokes:
        raise ValueError("没有笔画，无法分段")
    if target > len(strokes):
        raise ValueError(f"字数 {target} 多于笔画数 {len(strokes)}，无法为每个字分配至少一笔")

    gaps = stroke_gaps(strokes, stroke_start_t)
    ranges = segment_by_gap(strokes, stroke_start_t, gap_ms)

    # 段数过多：合并「分界间隔」最小的相邻段，直到段数等于字数。
    while len(ranges) > target:
        best_k = 0
        best_gap = gaps[ranges[0][1] - 1]
        for k in range(1, len(ranges) - 1):
            boundary_gap = gaps[ranges[k][1] - 1]
            if boundary_gap < best_gap:
                best_gap, best_k = boundary_gap, k
        merged = (ranges[best_k][0], ranges[best_k + 1][1])
        ranges[best_k : best_k + 2] = [merged]

    # 段数不足：拆分「内部间隔」最大的段，直到段数等于字数。
    while len(ranges) < target:
        best_k: int | None = None
        best_at: int | None = None
        best_gap = -1.0
        for k, (start, end) in enumerate(ranges):
            if end - start < 2:
                continue
            for i in range(start, end - 1):
                if gaps[i] > best_gap:
                    best_gap, best_k, best_at = gaps[i], k, i
        if best_k is None or best_at is None:
            raise ValueError("可用笔画不足以拆分成目标字数")
        start, end = ranges[best_k]
        ranges[best_k : best_k + 1] = [(start, best_at + 1), (best_at + 1, end)]

    return ranges
