"""命令行入口。

阶段 1 的目标是**验证硬件链路**：文本 → 标准笔顺骨架 → G 代码 → 写字机。
这条链路不依赖任何 AI 模型，因此可以先把机械精度、笔压一致性、
串口时序这些真正的风险暴露出来。

用法::

    # 默认按横条本排版（150×213 mm / 22 行，字压在横线上）
    python -m handwriting.cli render "春眠不觉晓" -o chun.nc --svg chun.svg

    # 自由版面（导到空白纸）
    python -m handwriting.cli render "春眠不觉晓" --mode grid --char-size 20

    # 预取字形数据（需要联网；之后可加 --offline 离线使用）
    python -m handwriting.cli preload "春眠不觉晓"

    # 只看统计，不写文件
    python -m handwriting.cli render "永" --dry-run

    # 体检采集数据并打印风格特征（自动识别单字 1.0 / 整行 2.0）
    python -m handwriting.cli inspect capture.json
"""

from __future__ import annotations

import argparse
import contextlib
import json
import sys
from collections import Counter
from pathlib import Path

from handwriting.dataset import CaptureData
from handwriting.gcode import PlotSettings, glyphs_to_gcode_with_stats
from handwriting.glyph import DEFAULT_CACHE_DIR, HanziWriterSource
from handwriting.layout import (
    NOTEBOOK_HEIGHT_MM,
    NOTEBOOK_LINES,
    NOTEBOOK_WIDTH_MM,
    LayoutSettings,
    Mode,
    compute,
    resolve_char_size,
    resolve_chars_per_line,
)
from handwriting.line import LineCapture
from handwriting.preview import SvgStyle, to_svg
from handwriting.render import RenderSettings, render_text
from handwriting.style import (
    LineStyleFeatures,
    StyleFeatures,
    extract,
    mean_features,
    mean_line_features,
)

# GRBL @ 115200 baud 的实测吞吐。这是整条链路的真实瓶颈，
# 计算只占端到端耗时的 0.02%（见 benchmarks/）。
GRBL_LINES_PER_SECOND = 380.0

# 采集数据格式（与 docs/data-format.md 对应）
CHAR_FORMAT = "handwriting-capture/1.0"  # 单字
LINE_FORMAT = "handwriting-capture/2.0"  # 整行

EXIT_OK = 0
EXIT_ERROR = 1

# 风格特征的中文标签（用于 inspect 输出）
_CHAR_LABELS = {
    "aspect_ratio": "字宽高比",
    "slant_deg": "主倾角(°)",
    "ink_density": "笔画密度",
    "straightness": "笔画平直度",
    "turn_mean_deg": "平均转折角(°)",
    "corner_deg": "折角(°)",
    "corner_rate": "折笔密度",
    "stroke_length_cv": "笔画长短差异",
    "pressure_mean": "平均压力",
    "pressure_std": "压力波动",
    "pressure_taper": "提按(中段−首尾)",
    "speed_mean": "书写速度",
    "speed_std": "速度波动",
    "pause_ratio": "抬笔时间占比",
}
_LINE_LABELS = {
    "char_width_cv": "字宽变异系数",
    "char_height_cv": "字高变异系数",
    "char_gap_mean": "平均字距",
    "char_gap_cv": "字距变异系数",
    "baseline_drift": "基线漂移",
    "baseline_slope": "基线趋势(正=下沉)",
    "char_pause_ratio": "字间停顿占比",
    "connection_rate": "连笔率",
    "slant_deg": "整行倾角(°)",
}


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

    page = render.add_argument_group("版面")
    page.add_argument(
        "--mode",
        choices=["notebook", "grid"],
        default="notebook",
        help="notebook=横条本（字压横线），grid=自由版面。默认 notebook",
    )
    page.add_argument("--char-size", type=float, default=0.0, help="字宽 mm，0 为自动")
    page.add_argument("--chars-per-line", type=int, default=0, help="每行字数，0 为自动")

    book = render.add_argument_group("横条本模式（单位 mm）")
    book.add_argument("--page-width", type=float, default=NOTEBOOK_WIDTH_MM, help="书写区宽")
    book.add_argument("--page-height", type=float, default=NOTEBOOK_HEIGHT_MM, help="书写区高")
    book.add_argument("--lines", type=int, default=NOTEBOOK_LINES, help="横线行数")
    book.add_argument("--first-line", type=int, default=1, help="从第几行起（1 起）")

    grid = render.add_argument_group("自由模式（单位 mm）")
    grid.add_argument("--char-gap", type=float, default=3.0, help="字间距")
    grid.add_argument("--line-gap", type=float, default=10.0, help="行间距")
    grid.add_argument("--margin", type=float, default=10.0, help="页边距")

    style = render.add_argument_group("字形风格")
    style.add_argument("--slant", type=float, default=0.0, help="倾斜角，右倾为正，单位度")
    style.add_argument("--jitter", type=float, default=0.0, help="抖动强度，建议 ≤ 0.005")
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

    inspect = subparsers.add_parser(
        "inspect", help="体检采集数据并打印风格特征（单字 1.0 / 整行 2.0 自动识别）"
    )
    inspect.add_argument("path", type=Path, help="采集导出的 JSON 文件")

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


def _print_problems(problems: list[str]) -> None:
    if not problems:
        print("  通过，未发现问题。")
        return
    print(f"  发现 {len(problems)} 个问题：")
    for problem in problems:
        print(f"    - {problem}")


def _print_features(features: StyleFeatures | LineStyleFeatures, labels: dict[str, str]) -> None:
    for name in features.numeric_fields:
        value = getattr(features, name)
        label = labels.get(name, name)
        print(f"  {label}：—（不可用）" if value is None else f"  {label}：{value:.4f}")


def _inspect_chars(capture: CaptureData) -> int:
    meta = capture.meta
    print(f"设备    ：{meta.device}")
    print(f"压感    ：{'支持' if meta.pressure_supported else '不支持'}")
    print(f"采样率  ：{meta.sampling_hz if meta.sampling_hz else '未知'}")
    print(f"样本数  ：{capture.char_count} 字")
    print(f"采样点  ：{capture.total_points}（平均 {capture.average_points_per_char:.0f}/字）")

    print()
    print("== 质量检查 ==")
    problems = capture.validate()
    _print_problems(problems)

    print()
    print("== 风格特征（全字表平均）==")
    features = []
    failed = 0
    for sample in capture.samples:
        try:
            features.append(extract(sample.to_stroke_set(), sample.stroke_start_t))
        except ValueError:
            failed += 1
    if features:
        _print_features(mean_features(features), _CHAR_LABELS)
        if failed:
            print(f"  （{failed} 个样本无法提取特征，已跳过）")
    else:
        print("  （没有可用于提取特征的样本）")

    return EXIT_OK if not problems else EXIT_ERROR


def _inspect_lines(capture: LineCapture) -> int:
    meta = capture.meta
    per_line = capture.total_points / capture.line_count if capture.line_count else 0.0
    print(f"设备    ：{meta.device}")
    print(f"压感    ：{'支持' if meta.pressure_supported else '不支持'}")
    print(f"采样率  ：{meta.sampling_hz if meta.sampling_hz else '未知'}")
    print(f"行数    ：{capture.line_count}")
    print(f"总字数  ：{capture.total_chars}")
    print(f"采样点  ：{capture.total_points}（平均 {per_line:.0f}/行）")

    print()
    print("== 质量检查 ==")
    problems = capture.validate()
    _print_problems(problems)

    print()
    print("== 行级风格特征（全部行平均）==")
    features = []
    failed = 0
    for line in capture.lines:
        try:
            features.append(line.style_features())
        except ValueError:
            failed += 1
    if features:
        _print_features(mean_line_features(features), _LINE_LABELS)
        if failed:
            print(f"  （{failed} 行无法提取特征，已跳过）")
    else:
        print("  （没有可用于提取特征的行）")

    return EXIT_OK if not problems else EXIT_ERROR


def _cmd_inspect(args: argparse.Namespace) -> int:
    path: Path = args.path
    if not path.is_file():
        print(f"错误：找不到文件 {path}")
        return EXIT_ERROR

    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        print(f"错误：无法读取或解析 {path}：{error}")
        return EXIT_ERROR

    fmt = payload.get("format")
    print("== 采集数据体检 ==")
    print(f"文件    ：{path}")
    print(f"格式    ：{fmt}")
    print()

    if fmt == LINE_FORMAT:
        return _inspect_lines(LineCapture.from_dict(payload, source=path))
    if fmt == CHAR_FORMAT:
        return _inspect_chars(CaptureData.from_dict(payload, source=path))

    print(f"错误：无法识别的数据格式 {fmt!r}（期望 {CHAR_FORMAT} 或 {LINE_FORMAT}）")
    return EXIT_ERROR


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
    layout_settings = LayoutSettings(
        mode=Mode.NOTEBOOK if args.mode == "notebook" else Mode.GRID,
        page_width_mm=args.page_width,
        page_height_mm=args.page_height,
        char_size_mm=args.char_size,
        chars_per_line=args.chars_per_line,
        line_count=args.lines,
        first_line=args.first_line,
        char_gap_mm=args.char_gap,
        line_gap_mm=args.line_gap,
        margin_x_mm=args.margin,
        margin_y_mm=args.margin,
    )
    plot_settings = PlotSettings(
        char_size_mm=args.char_size if args.char_size > 0 else 20.0,
        char_gap_mm=args.char_gap,
        line_gap_mm=args.line_gap,
        chars_per_line=resolve_chars_per_line(layout_settings),
        margin_x_mm=args.margin,
        margin_y_mm=args.margin,
        pen_up_z=args.pen_up_z,
        pen_down_z=args.pen_down_z,
        use_z_axis=not args.spindle,
        feed_draw=args.feed_draw,
        feed_travel=args.feed_travel,
    )

    # chars_per_line 传 0：折行统一由 layout 负责，避免两处各折一次
    result = render_text(text, source=source, settings=render_settings, chars_per_line=0)

    if result.is_empty:
        print("错误：没有任何字符被渲染。字形库可能不可用，或文本里没有可书写的字符。")
        if result.missing:
            print(f"缺失字符：{''.join(result.missing)}")
        return EXIT_ERROR

    row_counts = [len(row) for row in result.rows]
    page = compute(row_counts, layout_settings)
    glyphs = [glyph for row in result.rows for glyph in row]

    gcode_result = glyphs_to_gcode_with_stats(glyphs, page, plot_settings)
    program = gcode_result.lines_text()
    stats = gcode_result.stats

    serial_seconds = stats.line_count / GRBL_LINES_PER_SECOND
    char_size = resolve_char_size(layout_settings)

    print("== 渲染统计 ==")
    print(f"字符数      : {len(glyphs)}")
    print(f"笔画数      : {stats.stroke_count}")
    print(f"采样点      : {stats.point_count}")
    print(f"G 代码行数  : {stats.line_count}")
    print()
    mode_label = "横条本" if layout_settings.mode is Mode.NOTEBOOK else "自由版面"
    print(f"版面        : {mode_label}  {page.width_mm:.1f} × {page.height_mm:.1f} mm")
    print(f"             每行 {page.chars_per_line} 字，字宽 {char_size:.2f} mm")
    if page.rule_lines_mm:
        pitch = page.height_mm / layout_settings.line_count
        print(f"             横线 {len(page.rule_lines_mm)} 条，行距 {pitch:.2f} mm")
    print()
    print(f"绘制路径    : {stats.draw_length_mm:.1f} mm")
    print(f"空行程      : {stats.travel_length_mm:.1f} mm")
    print(f"运动时间    : {stats.estimated_seconds:.1f} s（仅按进给速度估算）")
    print(
        f"串口传输    : {serial_seconds:.1f} s"
        f"（{stats.line_count} 行 @ {GRBL_LINES_PER_SECOND:.0f} 行/秒）"
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
        svg = to_svg(glyphs, page, SvgStyle())
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
    if args.command == "inspect":
        return _cmd_inspect(args)

    parser.error(f"未知子命令 {args.command!r}")
    return EXIT_ERROR  # pragma: no cover - argparse 会先退出


if __name__ == "__main__":
    raise SystemExit(main())
