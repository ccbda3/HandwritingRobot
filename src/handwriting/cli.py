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
import re
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
from handwriting.shapes import compose_mindmap, compose_table, svg_to_stroke_set
from handwriting.sheets import ShapeSheet
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
    # 默认值取自本机（奎享雕刻）实测配置：书写 8000、抬笔 Z=0、落笔 Z=6。
    # 注意这台机器 Z 越大越往下压，与「Z 向上为正」的常见约定相反，按自己机器实测。
    machine.add_argument("--feed-draw", type=float, default=8000.0, help="书写进给 mm/min")
    machine.add_argument("--feed-travel", type=float, default=3000.0, help="空行程进给 mm/min")
    machine.add_argument("--pen-up-z", type=float, default=0.0, help="抬笔 Z 值")
    machine.add_argument("--pen-down-z", type=float, default=6.0, help="落笔 Z 值")
    machine.add_argument(
        "--invert-y",
        action="store_true",
        help="Y 轴取反（机器 Y+ 朝上、纸上向下走是 -Y 时使用）",
    )
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

    # ---- compose：任意图形（表格 / 思维导图 / SVG 导入）-------------------
    compose = subparsers.add_parser(
        "compose", help="生成任意图形（表格 / 思维导图 / SVG 导入）的 G 代码"
    )
    machine = compose.add_argument_group("机器参数")
    machine.add_argument("--feed-draw", type=float, default=8000.0, help="书写进给 mm/min")
    machine.add_argument("--feed-travel", type=float, default=3000.0, help="空行程进给 mm/min")
    machine.add_argument("--pen-up-z", type=float, default=0.0, help="抬笔 Z 值")
    machine.add_argument("--pen-down-z", type=float, default=6.0, help="落笔 Z 值")
    machine.add_argument("--invert-y", action="store_true", help="Y 轴取反（机器 Y+ 朝上时）")
    machine.add_argument("--spindle", action="store_true", help="用主轴 PWM 抬落笔而非 Z 轴")

    page = compose.add_argument_group("版面")
    page.add_argument("--mode", choices=["notebook", "grid"], default="grid", help="默认 grid")
    page.add_argument("--page-width", type=float, default=NOTEBOOK_WIDTH_MM)
    page.add_argument("--page-height", type=float, default=NOTEBOOK_HEIGHT_MM)
    page.add_argument("--lines", type=int, default=NOTEBOOK_LINES)
    page.add_argument("--char-size", type=float, default=0.0, help="每个图形的单元格边长 mm")
    page.add_argument("--chars-per-line", type=int, default=0, help="每行放几个图形，0 为自动")
    page.add_argument("--char-gap", type=float, default=3.0)
    page.add_argument("--line-gap", type=float, default=10.0)
    page.add_argument("--margin", type=float, default=10.0)

    kind = compose.add_subparsers(dest="kind", required=True)
    table = kind.add_parser("table", help="表格")
    table.add_argument("--rows", type=int, required=True)
    table.add_argument("--cols", type=int, required=True)
    table.add_argument(
        "--text",
        help="单元格文字：行用 ; 分隔，单元格用 , 或制表符（支持中文逗号）分隔",
    )
    table.add_argument("--cache-dir", type=Path, default=DEFAULT_CACHE_DIR)
    table.add_argument("--offline", action="store_true", help="只用本地缓存")

    mind = kind.add_parser("mindmap", help="思维导图")
    mind.add_argument(
        "--nodes", required=True, help="节点：id:x,y,w,h,标签;...（坐标为 0~1 归一化）"
    )
    mind.add_argument("--edges", required=True, help="连线：a,b;...")
    mind.add_argument("--cache-dir", type=Path, default=DEFAULT_CACHE_DIR)
    mind.add_argument("--offline", action="store_true")

    svgc = kind.add_parser("svg", help="导入 SVG 矢量图")
    svgc.add_argument("--file", type=Path, required=True, help="SVG 文件路径")

    for sub in (table, mind, svgc):
        sub.add_argument("-o", "--output", type=Path, help="G 代码输出路径")
        sub.add_argument("--svg", type=Path, help="同时导出 SVG 预览")

    # ---- train：训练风格模型 ------------------------------------------------
    train_p = subparsers.add_parser("train", help="训练风格模型（不提供数据时用合成笔迹自测）")
    train_p.add_argument(
        "--data", type=Path, help="采集 JSON（1.0 单字 / 2.0 整行）；不填则用合成数据"
    )
    train_p.add_argument(
        "--align",
        action="store_true",
        help="把采集数据对齐到标准骨架再训练（真实数据建议开启）",
    )
    train_p.add_argument(
        "--chars",
        help="用字形骨架做基准（需本地缓存），如 '永西叶'；比随机曲线更接近真实笔画",
    )
    train_p.add_argument("--cache-dir", type=Path, default=DEFAULT_CACHE_DIR)
    train_p.add_argument("--offline", action="store_true")
    train_p.add_argument("--samples", type=int, default=300, help="合成笔画数量")
    train_p.add_argument("--epochs", type=int, default=300)
    train_p.add_argument("--hidden", type=int, default=128)
    train_p.add_argument("--mixtures", type=int, default=5)
    train_p.add_argument("--n-points", type=int, default=48)
    train_p.add_argument("--smooth", type=int, default=5)
    train_p.add_argument("--lr", type=float, default=1e-3)
    train_p.add_argument("--seed", type=int, default=0)
    train_p.add_argument("--device", choices=["auto", "cpu", "cuda"], default="auto")
    train_p.add_argument("--out", type=Path, default=Path("data/exports/style-model.pt"))

    # ---- stylize：用模型生成 ------------------------------------------------
    styl = subparsers.add_parser("stylize", help="用训练好的风格模型生成笔迹")
    styl.add_argument("text", nargs="?", help="要书写的文本")
    styl.add_argument("-f", "--file", type=Path, help="从文件读取文本")
    styl.add_argument("--model", type=Path, required=True, help="模型检查点 .pt")
    styl.add_argument("--svg-file", type=Path, help="改为给一个 SVG 图形加风格")
    styl.add_argument("-o", "--output", type=Path, help="G 代码输出路径")
    styl.add_argument("--svg", type=Path, help="同时导出 SVG 预览")
    styl.add_argument("--device", choices=["auto", "cpu", "cuda"], default="auto")
    styl.add_argument(
        "--sample",
        action="store_true",
        help="逐点随机采样（默认取混合均值，更平滑可复现）",
    )

    s_page = styl.add_argument_group("版面")
    s_page.add_argument("--mode", choices=["notebook", "grid"], default="notebook")
    s_page.add_argument("--page-width", type=float, default=NOTEBOOK_WIDTH_MM)
    s_page.add_argument("--page-height", type=float, default=NOTEBOOK_HEIGHT_MM)
    s_page.add_argument("--lines", type=int, default=NOTEBOOK_LINES)
    s_page.add_argument("--char-size", type=float, default=0.0)
    s_page.add_argument("--chars-per-line", type=int, default=0)
    s_page.add_argument("--char-gap", type=float, default=3.0)
    s_page.add_argument("--line-gap", type=float, default=10.0)
    s_page.add_argument("--margin", type=float, default=10.0)

    s_machine = styl.add_argument_group("机器参数")
    s_machine.add_argument("--feed-draw", type=float, default=8000.0)
    s_machine.add_argument("--feed-travel", type=float, default=3000.0)
    s_machine.add_argument("--pen-up-z", type=float, default=0.0)
    s_machine.add_argument("--pen-down-z", type=float, default=6.0)
    s_machine.add_argument("--invert-y", action="store_true")
    s_machine.add_argument("--spindle", action="store_true")

    s_source = styl.add_argument_group("字形数据源")
    s_source.add_argument("--cache-dir", type=Path, default=DEFAULT_CACHE_DIR)
    s_source.add_argument("--offline", action="store_true")

    # ---- export-weights：训练检查点 → C++ 可读 JSON ------------------------
    export_p = subparsers.add_parser("export-weights", help="把训练检查点导出为 C++ 可读的 JSON")
    export_p.add_argument("--model", type=Path, required=True)
    export_p.add_argument(
        "-o", "--output", type=Path, default=Path("data/exports/style-model.json")
    )

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
        invert_y=args.invert_y,
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


def _parse_table_text(text: str) -> list[list[str]]:
    """行用 ; 分隔，单元格用 , 或制表符（含中文逗号）分隔。"""
    rows: list[list[str]] = []
    for row in text.split(";"):
        row = row.strip()
        if not row:
            continue
        cells = [c.strip() for c in re.split(r"[,，\t]", row) if c.strip()]
        rows.append(cells)
    return rows


def _parse_mindmap(nodes_str: str, edges_str: str) -> tuple[list, list]:
    from handwriting.shapes import MindEdge, MindNode

    nodes: list[MindNode] = []
    for part in nodes_str.split(";"):
        part = part.strip()
        if not part:
            continue
        nid, sep, rest = part.partition(":")
        if not sep:
            raise SystemExit(f"节点格式错误（应为 id:x,y,w,h,标签）：{part!r}")
        vals = rest.split(",", 4)  # x, y, w, h, 标签（标签可含逗号）
        if len(vals) < 4:
            raise SystemExit(f"节点格式错误（应为 id:x,y,w,h,标签）：{part!r}")
        label = vals[4].strip() if len(vals) >= 5 else ""
        nodes.append(
            MindNode(
                id=nid.strip(),
                x=float(vals[0]),
                y=float(vals[1]),
                w=float(vals[2]),
                h=float(vals[3]),
                label=label,
            )
        )

    edges: list[MindEdge] = []
    for part in edges_str.split(";"):
        part = part.strip()
        if not part:
            continue
        a, _, b = part.partition(",")
        edges.append(MindEdge(a.strip(), b.strip()))
    return nodes, edges


def _cmd_compose(args: argparse.Namespace) -> int:
    # 仅当涉及文字（表格单元格 / 节点标签）时才需要字形数据源
    source = None
    if getattr(args, "text", None) or args.kind == "mindmap":
        source = HanziWriterSource(cache_dir=args.cache_dir, offline=args.offline)

    if args.kind == "table":
        texts = _parse_table_text(args.text) if args.text else None
        shapes = [compose_table(args.rows, args.cols, texts, source)]
    elif args.kind == "mindmap":
        nodes, edges = _parse_mindmap(args.nodes, args.edges)
        shapes = [compose_mindmap(nodes, edges, source)]
    elif args.kind == "svg":
        if not args.file.is_file():
            print(f"错误：找不到 SVG 文件 {args.file}")
            return EXIT_ERROR
        svg_text = args.file.read_text(encoding="utf-8")
        shapes = [svg_to_stroke_set(svg_text)]
    else:  # pragma: no cover - argparse 约束
        raise SystemExit(f"未知图形类型 {args.kind!r}")

    plot = PlotSettings(
        pen_up_z=args.pen_up_z,
        pen_down_z=args.pen_down_z,
        use_z_axis=not args.spindle,
        feed_draw=args.feed_draw,
        feed_travel=args.feed_travel,
        invert_y=args.invert_y,
    )
    layout = LayoutSettings(
        mode=Mode.NOTEBOOK if args.mode == "notebook" else Mode.GRID,
        page_width_mm=args.page_width,
        page_height_mm=args.page_height,
        char_size_mm=args.char_size,
        chars_per_line=args.chars_per_line,
        line_count=args.lines,
        char_gap_mm=args.char_gap,
        line_gap_mm=args.line_gap,
        margin_x_mm=args.margin,
        margin_y_mm=args.margin,
    )

    sheet = ShapeSheet(shapes)
    fitted, page = sheet.layout(layout)
    gres = glyphs_to_gcode_with_stats(fitted, page, plot)
    stats = gres.stats
    program = gres.lines_text()
    svg = to_svg(fitted, page, SvgStyle())

    print("== 图形合成统计 ==")
    print(f"图形数      : {sheet.count}")
    print(f"笔画数      : {stats.stroke_count}")
    print(f"采样点      : {stats.point_count}")
    print(f"G 代码行数  : {stats.line_count}")
    print(f"版面        : {page.width_mm:.1f} × {page.height_mm:.1f} mm")
    print(f"绘制路径    : {stats.draw_length_mm:.1f} mm")
    print(f"空行程      : {stats.travel_length_mm:.1f} mm")
    serial_seconds = stats.line_count / GRBL_LINES_PER_SECOND
    print(
        f"串口传输    : {serial_seconds:.1f} s"
        f"（{stats.line_count} 行 @ {GRBL_LINES_PER_SECOND:.0f} 行/秒）"
    )

    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(program, encoding="utf-8")
        print(f"\n已写入 G 代码：{args.output}")
    if args.svg is not None:
        args.svg.parent.mkdir(parents=True, exist_ok=True)
        args.svg.write_text(svg, encoding="utf-8")
        print(f"已写入 SVG 预览：{args.svg}")
    if args.output is None and args.svg is None:
        print("\n== G 代码 ==")
        sys.stdout.write(program)

    return EXIT_OK


def _resolve_device(value: str) -> str:
    from handwriting.model.train import default_device

    return default_device() if value == "auto" else value


def _capture_pairs(path: Path, source: HanziWriterSource) -> list[tuple]:
    """从 1.0/2.0 采集文件里提取 (采集字, 标准骨架) 对。"""
    payload = json.loads(path.read_text(encoding="utf-8"))
    fmt = payload.get("format")
    pairs: list[tuple] = []

    if fmt == "handwriting-capture/1.0":
        data = CaptureData.from_dict(payload, source=path)
        for sample in data.samples:
            if not sample.char or not sample.strokes:
                continue
            try:
                reference = source.load(sample.char).to_stroke_set()
            except (LookupError, OSError, ValueError):
                continue
            pairs.append((sample.to_stroke_set(fit=True), reference))
    elif fmt == "handwriting-capture/2.0":
        data = LineCapture.from_dict(payload, source=path)
        for line in data.lines:
            try:
                chars = line.split_chars(fit=True)
            except ValueError:
                continue
            for char, captured in chars:
                if not char or not captured.strokes:
                    continue
                try:
                    reference = source.load(char).to_stroke_set()
                except (LookupError, OSError, ValueError):
                    continue
                pairs.append((captured, reference))
    else:
        raise SystemExit(f"不支持的数据格式：{fmt!r}")

    return pairs


def _cmd_train(args: argparse.Namespace) -> int:
    from handwriting.model.data import build_examples
    from handwriting.model.synthetic import synthetic_pairs
    from handwriting.model.train import TrainConfig, save_checkpoint, train

    if args.align and args.data is None:
        print("错误：--align 需要配合 --data 使用")
        return EXIT_ERROR

    if args.data is not None:
        if not args.data.is_file():
            print(f"错误：找不到数据文件 {args.data}")
            return EXIT_ERROR
        if args.align:
            from handwriting.model.align import build_aligned_examples

            glyph_source = HanziWriterSource(cache_dir=args.cache_dir, offline=args.offline)
            pairs = _capture_pairs(args.data, glyph_source)
            if not pairs:
                print("错误：没有可对齐的样本（检查字形缓存或 char_ranges）")
                return EXIT_ERROR
            examples = build_aligned_examples(pairs, args.n_points, args.smooth)
            source_label = f"{args.data}（已对齐）"
        else:
            payload = json.loads(args.data.read_text(encoding="utf-8"))
            if payload.get("format") == "handwriting-capture/1.0":
                print("错误：1.0 单字数据请加 --align（需要对齐到标准骨架）")
                return EXIT_ERROR
            capture = LineCapture.load(args.data)
            strokes = [stroke for line in capture.lines for stroke in line.strokes]
            examples = build_examples(strokes, args.n_points, args.smooth)
            source_label = str(args.data)
    elif args.chars:
        from handwriting.model.synthetic import glyph_pairs

        glyph_source = HanziWriterSource(cache_dir=args.cache_dir, offline=args.offline)
        pairs = glyph_pairs(args.chars, source=glyph_source, n_points=args.n_points, seed=args.seed)
        examples = build_examples(
            [styled for _, styled in pairs],
            args.n_points,
            args.smooth,
            centerlines=[base for base, _ in pairs],
        )
        source_label = f"glyphs:{args.chars}"
    else:
        pairs = synthetic_pairs(count=args.samples, n_points=args.n_points, seed=args.seed)
        examples = build_examples(
            [styled for _, styled in pairs],
            args.n_points,
            args.smooth,
            centerlines=[base for base, _ in pairs],
        )
        source_label = "synthetic"

    if not examples:
        print("错误：没有可用的训练样本")
        return EXIT_ERROR

    config = TrainConfig(
        n_points=args.n_points,
        smooth_window=args.smooth,
        hidden=args.hidden,
        mixtures=args.mixtures,
        epochs=args.epochs,
        lr=args.lr,
        seed=args.seed,
        device=_resolve_device(args.device),
    )
    print(f"训练样本：{len(examples)} 笔（来源：{source_label}）")
    print(f"设备：{config.device}  轮数：{config.epochs}")

    every = max(1, config.epochs // 10)

    def progress(epoch: int, loss: float) -> None:
        if epoch == 0 or (epoch + 1) % every == 0 or epoch == config.epochs - 1:
            print(f"  epoch {epoch + 1:4d}/{config.epochs}  loss {loss:.4f}")

    model, history = train(examples, config, progress=progress)
    path = save_checkpoint(
        model,
        config,
        args.out,
        meta={"source": source_label, "examples": len(examples)},
    )
    print(f"损失：{history[0]:.4f} → {history[-1]:.4f}")
    print(f"已写入模型：{path}")
    return EXIT_OK


def _cmd_stylize(args: argparse.Namespace) -> int:
    from handwriting.model.generate import stylize_stroke_set
    from handwriting.model.train import load_checkpoint

    if not args.model.is_file():
        print(f"错误：找不到模型 {args.model}")
        return EXIT_ERROR

    device = _resolve_device(args.device)
    model, config, _meta = load_checkpoint(args.model, device)

    plot = PlotSettings(
        pen_up_z=args.pen_up_z,
        pen_down_z=args.pen_down_z,
        use_z_axis=not args.spindle,
        feed_draw=args.feed_draw,
        feed_travel=args.feed_travel,
        invert_y=args.invert_y,
    )
    layout = LayoutSettings(
        mode=Mode.NOTEBOOK if args.mode == "notebook" else Mode.GRID,
        page_width_mm=args.page_width,
        page_height_mm=args.page_height,
        char_size_mm=args.char_size,
        chars_per_line=args.chars_per_line,
        line_count=args.lines,
        char_gap_mm=args.char_gap,
        line_gap_mm=args.line_gap,
        margin_x_mm=args.margin,
        margin_y_mm=args.margin,
    )

    if args.svg_file is not None:
        if not args.svg_file.is_file():
            print(f"错误：找不到 SVG 文件 {args.svg_file}")
            return EXIT_ERROR
        from handwriting.shapes import svg_to_stroke_set
        from handwriting.sheets import ShapeSheet

        target = svg_to_stroke_set(args.svg_file.read_text(encoding="utf-8"))
        stylized = stylize_stroke_set(model, config, target, sample=args.sample)
        fitted, page = ShapeSheet([stylized]).layout(layout)
        gres = glyphs_to_gcode_with_stats(fitted, page, plot)
        svg = to_svg(fitted, page, SvgStyle())
    else:
        text = _read_text(args)
        source = HanziWriterSource(cache_dir=args.cache_dir, offline=args.offline)
        result = render_text(text, source=source, chars_per_line=0)
        if result.is_empty:
            print("错误：没有任何字符被渲染")
            return EXIT_ERROR
        rows = [
            [stylize_stroke_set(model, config, glyph, sample=args.sample) for glyph in row]
            for row in result.rows
        ]
        page = compute([len(row) for row in rows], layout)
        glyphs = [glyph for row in rows for glyph in row]
        gres = glyphs_to_gcode_with_stats(glyphs, page, plot)
        svg = to_svg(glyphs, page, SvgStyle())

    stats = gres.stats
    print("== 风格化统计 ==")
    print(f"笔画数      : {stats.stroke_count}")
    print(f"采样点      : {stats.point_count}")
    print(f"G 代码行数  : {stats.line_count}")
    print(f"版面        : {page.width_mm:.1f} × {page.height_mm:.1f} mm")
    print(f"绘制路径    : {stats.draw_length_mm:.1f} mm")

    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(gres.lines_text(), encoding="utf-8")
        print(f"\n已写入 G 代码：{args.output}")
    if args.svg is not None:
        args.svg.parent.mkdir(parents=True, exist_ok=True)
        args.svg.write_text(svg, encoding="utf-8")
        print(f"已写入 SVG 预览：{args.svg}")
    if args.output is None and args.svg is None:
        sys.stdout.write(gres.lines_text())

    return EXIT_OK


def _cmd_export_weights(args: argparse.Namespace) -> int:
    """训练检查点 → C++ 可读 JSON（ADR-0007 的权重交接点）。"""
    from handwriting.model.data import RESIDUAL_SCALE
    from handwriting.model.train import load_checkpoint

    if not args.model.is_file():
        print(f"错误：找不到模型 {args.model}")
        return EXIT_ERROR
    model, config, meta = load_checkpoint(args.model, "cpu")

    def layer(linear) -> dict:
        return {
            "weight": linear.weight.detach().tolist(),
            "bias": linear.bias.detach().tolist(),
        }

    payload = {
        "format": "handwriting-style-model/1",
        "config": {
            "n_points": config.n_points,
            "smooth_window": config.smooth_window,
            "hidden": config.hidden,
            "mixtures": config.mixtures,
            "residual_scale": RESIDUAL_SCALE,
        },
        "layers": [layer(model.trunk[0]), layer(model.trunk[2])],
        "head": layer(model.out),
        "meta": meta,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(payload), encoding="utf-8")
    print(f"已导出权重：{args.output}（{args.output.stat().st_size // 1024} KB）")
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
    if args.command == "compose":
        return _cmd_compose(args)
    if args.command == "train":
        return _cmd_train(args)
    if args.command == "stylize":
        return _cmd_stylize(args)
    if args.command == "export-weights":
        return _cmd_export_weights(args)

    parser.error(f"未知子命令 {args.command!r}")
    return EXIT_ERROR  # pragma: no cover - argparse 会先退出


if __name__ == "__main__":
    raise SystemExit(main())
