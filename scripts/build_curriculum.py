"""生成覆盖度优化后的字表顺序，供采集器内嵌。

用法：
    python scripts/build_curriculum.py              # 打印
    python scripts/build_curriculum.py --write      # 写入 capture/order.txt
    python scripts/build_curriculum.py --report     # 附带覆盖度报告

采集器是纯前端单文件，不引入构建步骤，因此顺序在开发期算好、
以字符串常量内嵌进 HTML。改动字表或选字策略后需要重新运行本脚本。
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

from handwriting.curriculum import (  # noqa: E402
    CHARACTERS,
    COLD_START_CHARS,
    all_cells,
    cell_of,
    coverage_report,
    encode_cells,
    order_by_coverage,
)

DEFAULT_TARGET = ROOT / "capture" / "order.txt"


def main() -> int:
    parser = argparse.ArgumentParser(description="生成覆盖度优化的字表顺序")
    parser.add_argument("--write", action="store_true", help=f"写入 {DEFAULT_TARGET}")
    parser.add_argument("--out", type=Path, default=DEFAULT_TARGET, help="输出路径")
    parser.add_argument("--report", action="store_true", help="打印覆盖度报告")
    args = parser.parse_args()

    order = order_by_coverage()
    joined = "".join(order)

    print(f"字表长度：{len(order)}")
    print(f"冷启动前 {len(COLD_START_CHARS)} 字：{''.join(COLD_START_CHARS)}")
    print()
    print("前 20 字的结构-复杂度格子：")
    for index, char in enumerate(order[:20], start=1):
        meta = CHARACTERS[char]
        print(f"  {index:>3}. {char}  {meta.structure}  {meta.complexity_label}")
    print()

    if args.report:
        cells = Counter(cell_of(char) for char in order[:40])
        print("写完 40 字后的格子覆盖：")
        for cell in all_cells():
            print(f"  {cell[0]}/{cell[1]}: {cells[cell]}")
        print()
        print(json.dumps(coverage_report(list(order[:40])), ensure_ascii=False, indent=2))
        print()

    if args.write:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(joined, encoding="utf-8")
        print(f"已写入 {args.out}")
        print()

    print("== 内嵌进 capture/index.html 的两个常量 ==")
    print(f"ORDER = {joined}")
    print(f"CELLS = {encode_cells(order)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
