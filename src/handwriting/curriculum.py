"""字表元数据与主动选字策略。

## 为什么需要

固定字表顺序（比如按使用频率）在同样 150 字的预算下信息量偏低：
可能写了 60 个左右结构的字，却只写了 2 个包围结构，
导致模型在包围结构上几乎没有风格参考。

本模块按「结构 × 复杂度」的覆盖网格挑字，让样本池的风格覆盖最大化，
并且让**前 8 个字就能覆盖 8 个不同的结构—复杂度格子**，
满足 ADR-0005 冷启动的要求。

## 标注说明

``structure`` 与 ``complexity`` 为人工标注的**初版数据**，
用于跑通主动选字流程。待接入 Make Me a Hanzi 的字形数据后，
应改为从笔画数据自动推导并交叉校验。

修改本表时请同步检查 ``tests/test_curriculum.py`` 中的数量与分布断言。
"""

from __future__ import annotations

import re
from collections import Counter
from collections.abc import Iterable, Iterator, Sequence
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path

# 冷启动阶段要求用户写的字数。ADR-0005 假设 8 个字即可出可用结果。
COLD_START_SIZE = 8

# 冷启动字集：人工策展，不由算法生成。
#
# 选取标准有两条，缺一不可：
#   1. 覆盖 8 个**互不相同**的（结构, 复杂度）格子 —— 保证风格覆盖最大化
#   2. 书写体验自然 —— 用户头 8 个字就要有挫败感的话，采集根本进行不下去
#
# 「永」放在第一位是刻意的：永字八法包含横、竖、撇、捺、点、折、钩、提
# 全部八种基本笔画，一个字就能暴露用户在所有笔画类型上的书写习惯。
#
# 本集合的性质由 tests/test_curriculum.py 断言保护，修改后必须同步检查。
COLD_START_CHARS: tuple[str, ...] = ("永", "西", "叶", "好", "冬", "花", "国", "风")

# 建议完成量：达到后提示用户可以进入下一档
MILESTONES: tuple[int, ...] = (8, 40, 150)

STRUCTURES: tuple[str, ...] = ("独体", "左右", "上下", "包围", "半包围")

# 结构 → 单字符编码。采集器要把 150 个字的元数据内嵌进单文件 HTML，
# 用「结构码 + 复杂度」两字符表示一个字，整表只需 300 字符。
# 改动此处必须同步 capture/index.html 里的 STRUCTURES 映射表。
STRUCTURE_CODES: dict[str, str] = {
    "独体": "1",
    "左右": "2",
    "上下": "3",
    "包围": "4",
    "半包围": "5",
}

COMPLEXITY_LABELS: dict[int, str] = {
    1: "简单（≤5 画）",
    2: "中等（6~9 画）",
    3: "复杂（≥10 画）",
}


@dataclass(frozen=True, slots=True)
class CharacterMeta:
    """一个字的采集元数据。"""

    char: str
    structure: str
    complexity: int

    @property
    def cell(self) -> tuple[str, int]:
        """覆盖网格中的格子。"""
        return (self.structure, self.complexity)

    @property
    def complexity_label(self) -> str:
        return COMPLEXITY_LABELS[self.complexity]


_RAW_TABLE = """
# 字符 结构 复杂度（1: ≤5 画, 2: 6~9 画, 3: ≥10 画）
永 独体 1
一 独体 1
二 独体 1
三 独体 1
十 独体 1
木 独体 1
人 独体 1
大 独体 1
上 独体 1
小 独体 1
下 独体 1
口 独体 1
日 独体 1
月 独体 1
田 独体 1
目 独体 1
山 独体 1
川 独体 1
水 独体 1
火 独体 1
土 独体 1
石 半包围 1
金 上下 2
玉 独体 1
王 独体 1
生 独体 1
手 独体 1
心 独体 1
刀 独体 1
弓 独体 1
女 独体 1
子 独体 1
好 左右 2
明 左右 2
林 左右 2
森 上下 3
品 上下 2
晶 上下 3
中 独体 1
国 包围 2
民 独体 1
共 上下 2
和 左右 2
家 上下 3
天 独体 1
地 左右 2
春 上下 2
夏 上下 3
秋 左右 2
冬 上下 1
东 独体 1
南 上下 2
西 独体 2
北 左右 1
风 半包围 1
雨 独体 2
雪 上下 3
云 独体 1
花 上下 2
草 上下 2
树 左右 2
叶 左右 1
鸟 独体 1
鱼 上下 2
虫 独体 2
马 独体 1
牛 独体 1
羊 独体 2
犬 独体 1
猫 左右 3
龙 独体 1
虎 半包围 2
我 独体 2
你 左右 2
他 左右 1
她 左右 2
们 左右 1
是 上下 2
的 左右 2
了 独体 1
在 半包围 2
有 半包围 2
不 独体 1
就 左右 3
都 左右 3
很 左右 2
也 独体 1
与 独体 1
及 独体 1
或 半包围 2
但 左右 2
而 独体 2
因 包围 2
为 独体 1
所 左右 2
以 左右 1
这 半包围 2
那 左右 2
些 上下 2
什 左右 1
么 独体 1
谁 左右 3
哪 左右 2
里 独体 2
时 左右 2
候 左右 3
年 独体 2
分 上下 1
秒 左右 2
早 上下 2
晚 左右 3
前 上下 2
后 半包围 2
左 半包围 1
右 半包围 1
高 上下 3
低 左右 2
长 独体 1
短 左右 3
多 上下 2
少 独体 1
快 左右 2
慢 左右 3
新 左右 3
旧 左右 1
坏 左右 2
美 上下 2
丽 上下 2
亮 上下 2
暗 左右 3
冷 左右 2
热 上下 3
温 左右 3
暖 左右 3
开 独体 1
关 上下 2
进 半包围 2
出 独体 1
往 左右 2
来 独体 2
去 上下 1
走 上下 2
跑 左右 3
飞 独体 1
跳 左右 3
看 上下 2
听 左右 2
说 左右 2
读 左右 3
写 上下 1
"""


def _parse_table(raw: str) -> dict[str, CharacterMeta]:
    table: dict[str, CharacterMeta] = {}
    for line in raw.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = re.split(r"\s+", line)
        if len(parts) != 3:
            raise ValueError(f"字表行格式错误：{line!r}")
        char, structure, complexity_text = parts
        if structure not in STRUCTURES:
            raise ValueError(f"未知结构类型 {structure!r}（字符 {char}）")
        complexity = int(complexity_text)
        if complexity not in COMPLEXITY_LABELS:
            raise ValueError(f"未知复杂度 {complexity}（字符 {char}）")
        if char in table:
            raise ValueError(f"字表存在重复字符：{char}")
        table[char] = CharacterMeta(char=char, structure=structure, complexity=complexity)
    return table


CHARACTERS: dict[str, CharacterMeta] = _parse_table(_RAW_TABLE)
"""默认字表。键为字符，值为元数据。"""

DEFAULT_CHARACTERS: tuple[str, ...] = tuple(CHARACTERS)
"""按字表原始顺序排列的字符。"""


def cell_of(char: str) -> tuple[str, int]:
    """返回该字所在的覆盖网格格子。"""
    meta = CHARACTERS.get(char)
    if meta is None:
        raise KeyError(f"字表中没有 {char!r}")
    return meta.cell


def coverage(chars: Iterable[str]) -> Counter[tuple[str, int]]:
    """统计已采集字符在覆盖网格上的分布。"""
    return Counter(cell_of(char) for char in chars)


def all_cells() -> tuple[tuple[str, int], ...]:
    """字表中实际存在的所有格子（并非 5×3 全部存在）。"""
    return tuple(sorted({meta.cell for meta in CHARACTERS.values()}))


def coverage_report(collected: Sequence[str]) -> dict[str, object]:
    """生成人类可读的覆盖度报告，供 CLI 与 UI 使用。"""
    counts = coverage(collected)
    cells = all_cells()
    gaps = sorted(cells, key=lambda cell: (counts.get(cell, 0), cell))
    return {
        "collected": len(collected),
        "total": len(CHARACTERS),
        "counts": {f"{cell[0]}/{cell[1]}": counts.get(cell, 0) for cell in cells},
        "missing_cells": [f"{cell[0]}/{cell[1]}" for cell in cells if counts.get(cell, 0) == 0],
        "weakest_cells": [f"{cell[0]}/{cell[1]}" for cell in gaps[:3]],
        "milestone": next((m for m in MILESTONES if len(collected) < m), MILESTONES[-1]),
    }


def _selection_key(counts: Counter[tuple[str, int]], rank: dict[str, int]):
    """排序键：先补覆盖最少的格子，再优先简单字，最后按字表原序稳定排序。"""

    def key(char: str) -> tuple[int, int, int]:
        return (counts[cell_of(char)], CHARACTERS[char].complexity, rank[char])

    return key


def order_by_coverage(
    chars: Sequence[str] | None = None,
    cold_start: Sequence[str] | None = None,
) -> list[str]:
    """按风格覆盖度对字表重排。

    前 ``cold_start`` 个字取 :data:`COLD_START_CHARS`（人工策展），
    其余用轮转贪心补齐：每轮从「当前覆盖次数最少的格子」里取一个字，
    格子内优先取笔画少的（先易后难，用户不容易在中途受挫）。

    ``chars`` 只影响贪心补齐的部分；``cold_start`` 传空元组可跳过策展前缀，
    得到纯算法顺序（用于对比评估策展是否真的更好）。
    """
    pool = list(DEFAULT_CHARACTERS if chars is None else chars)
    if not pool:
        return []

    prefix = list(COLD_START_CHARS if cold_start is None else cold_start)
    unknown = [char for char in prefix if char not in pool]
    if unknown:
        raise KeyError(f"冷启动字不在字表中：{unknown}")

    rank = {char: index for index, char in enumerate(pool)}
    counts: Counter[tuple[str, int]] = Counter()
    for char in prefix:
        counts[cell_of(char)] += 1
    key = _selection_key(counts, rank)

    remaining = set(pool) - set(prefix)
    order: list[str] = list(prefix)
    while remaining:
        chosen = min(remaining, key=key)
        order.append(chosen)
        counts[cell_of(chosen)] += 1
        remaining.discard(chosen)
    return order


def next_char(collected: Sequence[str], skipped: Sequence[str] = ()) -> str | None:
    """推荐下一个该写的字。全部采完返回 ``None``。

    按 :func:`order_by_coverage` 预先算好的顺序返回第一个未采集的字，
    因此冷启动阶段严格遵循 :data:`COLD_START_CHARS` 的策展顺序。

    ``skipped`` 为用户主动跳过的字：它们不参与推荐，但也**不计入覆盖度**
    （否则跳过就等于采集，样本池会虚报进度）。

    每次调用只推荐一个字；采集器在用户写完一个字后重新调用，
    因此覆盖度随采集进度实时变化。
    """
    known = set(CHARACTERS)
    unknown = (set(collected) | set(skipped)) - known
    if unknown:
        raise KeyError(f"字表中没有这些字符：{sorted(unknown)}")

    exclude = set(collected) | set(skipped)
    for char in _default_order():
        if char not in exclude:
            return char
    return None


@lru_cache(maxsize=1)
def _default_order() -> tuple[str, ...]:
    """默认字表的覆盖度排序，进程内只算一次。"""
    return tuple(order_by_coverage())


def iter_meta(chars: Sequence[str]) -> Iterator[CharacterMeta]:
    for char in chars:
        meta = CHARACTERS.get(char)
        if meta is None:
            raise KeyError(f"字表中没有 {char!r}")
        yield meta


def encode_cells(chars: Sequence[str]) -> str:
    """把每个字的（结构, 复杂度）编码成两字符，拼成 ``len(chars) * 2`` 长度的串。"""
    return "".join(
        STRUCTURE_CODES[CHARACTERS[char].structure] + str(CHARACTERS[char].complexity)
        for char in chars
    )


def decode_cell(code: str) -> tuple[str, int]:
    """:func:`encode_cells` 的逆运算，输入两字符。"""
    if len(code) != 2:
        raise ValueError(f"编码必须是两字符，收到 {code!r}")
    for structure, digit in STRUCTURE_CODES.items():
        if digit == code[0]:
            complexity = int(code[1])
            if complexity not in COMPLEXITY_LABELS:
                raise ValueError(f"未知复杂度编码 {code!r}")
            return structure, complexity
    raise ValueError(f"未知结构编码 {code!r}")


def write_order(path: str | Path, chars: Sequence[str] | None = None) -> Path:
    """把排好序的字表写成一行字符串，供采集器内嵌。"""
    order = order_by_coverage(chars)
    target = Path(path)
    target.write_text("".join(order), encoding="utf-8")
    return target
