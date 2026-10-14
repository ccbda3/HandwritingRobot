"""命令行入口。

阶段 1 的目标是**验证硬件链路**：文本 → 标准笔顺骨架 → G 代码 → 写字机。
这条链路不依赖任何 AI 模型，因此可以先把机械精度、笔压一致性、
串口时序这些真正的风险暴露出来。

用法::

    # 生成 G 代码，同时导出 SVG 预览
    python -m handwriting.cli render "春眠不觉晓" -o chun.nc --svg chun.svg

    # 预取字形数据（需要联网；之后可加 --offline 离线使用）
    python -m handwriting.cli preload "春眠不觉晓"

    # 只看统计，不写文件
    python -m handwriting.cli render "永" --dry-run
"""

from __future__ import annotations

import argparse
import contextlib
import sys
from collections import Counter
from pathlib import Path

from handwriting.gcode import PlotSettings, strokes_to_gcode_with_stats
from handwriting.glyph import DEFAULT_CACHE_DIR, HanziWriterSource
from handwriting.preview import SvgStyle, to_svg
from handwriting.render import RenderSettings, render_text

# GRBL @ 115200 baud 的实测吞吐。这是整条链路的真实瓶颈，
# 计算只占端到端耗时的 0.02%（见 benchmarks/）。
GRBL_LINES_PER_SECOND = 380.0

EXIT_OK = 0
EXIT_ERROR = 1


def _configure_stdout() -> None:
    """Windows 控制台默认可能是 GBK，输出中文会抛 UnicodeEncodeError。"""
    stream = sys.stdout
    if hasattr(stream, "reconfigure"):
        with contextlib.suppress(OSError, ValueError):
            stream.reconfigure(encoding="utf-8", errors="replace")


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="handwriting",
        description="文本 → 标准笔顺骨架 → GRBL G 代码",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    render = subparsers.add_parser("render", help="渲染文本并生成 G 代码")
    render.add_argument("text", nargs="?", help="要书写的文本（也可用 -f 从文件读取）")
    render.add_argument("-f", "--file", type=Path, help="从文件读取文本")
    render.add_argument("-o", "--output", type=Path, help="G 代码输出路径（默认打印到 stdout）")
    render.add_argument("--svg", type=Path, help="同时导出 SVG 预览")
    render.add_argument("--dry-run", action="store_true", help="只统计，不写任何文件")

    layout = render.add_argument_group("版面（单位 mm）")
    layout.add_argument("--chars-per-line", type=int, default=10, help="每行字数，默认 10")
    layout.add_argument("--char-size", type=float, default=20.0, help="字格边长，默认 20")
    layout.add_argument("--char-gap", type=float, default=3.0, help="字间距，默认 3")
    layout.add_argument("--line-gap", type=float, default=10.0, help="行间距，默认 10")
    layout.add_argument("--margin", type=float, default=10.0, help="页边距，默认 10")

    style = render.add_argument_group("字形风格")
    style.add_argument("--slant", type=float, default=0.0, help="倾斜角，右倾为正，单位度")
    style.add_argument("--jitter", type=float, default=0.0, help="抖动强度，建议 ≤ 0.005，默认 0")
    style.add_argument("--jitter-seed", type=int, default=20261015, help="抖动随机种子")
    style.add_argument("--pressure-amp", type=float, default=0.30, help="提按幅度，0 为等压")
    style.add_argument("--writing-speed", type=float, default=2.5, help="合成时序用的书写速度")

    machine = render.add_argument_group("机器参数")
    machine.add_argument("--feed-draw", type=float, default=1200.0, help="书写进给 mm/min")
    machine.add_argument("--feed-travel", type=float, default=3000.0, help="空行程进给 mm/min")
    machine.add_argument("--pen-up-z", type=float, default=3.0, help="抬笔 Z 高度")
    machine.add_argument("--pen-down-z", type=float, default=0.0, help="落笔 Z 高度")
    machine.add_argument(
        "--spindle",
        action="store_true",
        help="用主轴 PWM（M3/M5）控制抬落笔，而不是 Z 轴",
    )

    source = render.add_argument_group("字形数据源")
    source.add_argument("--cache-dir", type=Path, default=DEFAULT_CACHE_DIR, help="字形缓存目录")
    source.add_argument("--offline", action="store_true", help="只用本地缓存，不联网")

    preload = subparsers.add_parser("preload", help="预取字形数据到本地缓存")
    preload.add_argument("text", help="要预取的文本（重复字符会自动去重）")
    preload.add_argument("--cache-dir", type=Path, default=DEFAULT_CACHE_DIR)
    preload.add_argument("--offline", action="store_true", help="只用本地缓存")

    return parser


def _read_text(args: argparse.Namespace) -> str:
    if args.file is not None:
        return args.file.read_text(encoding="utf-8")
    if args.text:
        return args.text
    raise SystemExit("错误：需要提供文本，或用 -f 指定文件")


def _cmd_preload(args: argparse.Namespace) -> int:
    unique = [char for char in dict.fromkeys(args.text) if not char.isspace()]
    source = HanziWriterSource(cache_dir=args.cache_dir, offline=args.offline)

    print(f"预取 {len(unique)} 个不重复字符 → {source.cache_dir}")
    failures = source.preload(unique)

    if failures:
        print(f"\n{len(failures)} 个字符失败：")
        for char, reason in failures.items():
            print(f"  {char}  {reason}")
        return EXIT_ERROR if len(failures) == len(unique) else EXIT_OK

    print("全部成功。")
    return EXIT_OK


def _cmd_render(args: argparse.Namespace) -> int:
    text = _read_text(args)

    source = HanziWriterSource(cache_dir=args.cache_dir, offline=args.offline)
    render_settings = RenderSettings(
        slant_deg=args.slant,
        jitter=args.jitter,
        jitter_seed=args.jitter_seed,
        pressure_amplitude=args.pressure_amp,
        writing_speed=args.writing_speed,
    )
    plot_settings = PlotSettings(
        char_size_mm=args.char_size,
        char_gap_mm=args.char_gap,
        line_gap_mm=args.line_gap,
        chars_per_line=args.chars_per_line,
        margin_x_mm=args.margin,
        margin_y_mm=args.margin,
        pen_up_z=args.pen_up_z,
        pen_down_z=args.pen_down_z,
        use_z_axis=not args.spindle,
        feed_draw=args.feed_draw,
        feed_travel=args.feed_travel,
    )

    result = render_text(
        text,
        source=source,
        settings=render_settings,
        chars_per_line=args.chars_per_line,
    )

    if result.is_empty:
        print("错误：没有任何字符被渲染。字形库可能不可用，或文本里没有可书写的字符。")
        if result.missing:
            print(f"缺失字符：{''.join(result.missing)}")
        return EXIT_ERROR

    gcode_result = strokes_to_gcode_with_stats(result.rows, plot_settings)
    program = gcode_result.lines_text()
    stats = gcode_result.stats

    serial_seconds = stats.line_count / GRBL_LINES_PER_SECOND

    print("== 渲染统计 ==")
    print(
        f"字符数      : {result.char_count}（{len(result.rows)} 行 × 最多 {args.chars_per_line} 字）"
    )
    print(f"笔画数      : {stats.stroke_count}")
    print(f"采样点      : {stats.point_count}")
    print(f"G 代码行数  : {stats.line_count}")
    print()
    print(f"绘制路径    : {stats.draw_length_mm:.1f} mm")
    print(f"空行程      : {stats.travel_length_mm:.1f} mm")
    print(f"运动时间    : {stats.estimated_seconds:.1f} s（仅按进给速度估算）")
    print(
        f"串口传输    : {serial_seconds:.1f} s（{stats.line_count} 行 @ {GRBL_LINES_PER_SECOND:.0f} 行/秒）"
    )
    print("              串口是本链路的真实瓶颈，不是计算")

    if result.missing:
        counts = Counter(char for char in text if char in set(result.missing))
        detail = "、".join(
            f"{char}×{count}" if count > 1 else char for char, count in counts.items()
        )
        print()
        print(f"警告：{len(result.missing)} 个字符未能渲染，已跳过：{detail}")
        for char in result.missing[:5]:
            print(f"      {char}：{result.failures.get(char, '未知原因')}")
        if len(result.missing) > 5:
            print(f"      …还有 {len(result.missing) - 5} 个，详见 preload 子命令")

    if args.dry_run:
        print()
        print("（--dry-run：未写入任何文件）")
        return EXIT_OK

    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(program, encoding="utf-8")
        print()
        print(f"已写入 G 代码：{args.output}")
    else:
        print()
        print("== G 代码 ==")
        sys.stdout.write(program)

    if args.svg is not None:
        svg = to_svg(result.rows, plot_settings, SvgStyle(show_grid=True))
        args.svg.parent.mkdir(parents=True, exist_ok=True)
        args.svg.write_text(svg, encoding="utf-8")
        print(f"已写入 SVG 预览：{args.svg}")

    return EXIT_OK


def main(argv: list[str] | None = None) -> int:
    _configure_stdout()
    parser = _build_parser()
    args = parser.parse_args(argv)

    if args.command == "render":
        return _cmd_render(args)
    if args.command == "preload":
        return _cmd_preload(args)

    parser.error(f"未知子命令 {args.command!r}")
    return EXIT_ERROR  # pragma: no cover - argparse 会先退出


if __name__ == "__main__":
    raise SystemExit(main())
