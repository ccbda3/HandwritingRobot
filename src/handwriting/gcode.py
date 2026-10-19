"""GRBL G 代码生成。

面向 GRBL 固件的写字机器人输出：
    G0  抬笔快速定位（空行程）
    G1  落笔书写（进给运动）

笔压控制两种方案：
    Z 轴方案   —— 用 Z 轴下沉量近似笔压，需配合弹簧笔，非线性，依赖标定表
    主轴方案   —— 用主轴 PWM（M3/M5）驱动电磁铁或舵机，开关式，无法连续调压

注意 GRBL 的固有限制（详见 docs/architecture.md）：
    - 梯形速度规划 + junction deviation，无 S 曲线加加速度限制
    - look-ahead 缓冲仅 16 段
    - 串口 115200 baud 下每秒约可下发 380 行，分段过密会导致规划器饥饿而出现停顿点
因此分段长度与进给速度需要在生成阶段就做好权衡，而不是交给固件。
"""

from __future__ import annotations

from collections.abc import Iterable, Sequence
from dataclasses import dataclass, field

from handwriting.layout import Page
from handwriting.stroke import Point, StrokeSet

DEFAULT_HEADER = ("G21", "G90", "G17")


@dataclass(slots=True)
class PlotSettings:
    """写字机与版面参数。"""

    # 版面
    char_size_mm: float = 20.0
    char_gap_mm: float = 3.0
    line_gap_mm: float = 10.0
    chars_per_line: int = 10
    margin_x_mm: float = 10.0
    margin_y_mm: float = 10.0

    # 抬落笔
    pen_up_z: float = 3.0
    pen_down_z: float = 0.0
    use_z_axis: bool = True
    pen_up_cmd: str = "M5"
    pen_down_cmd: str = "M3 S1000"

    # 运动
    feed_draw: float = 1200.0
    feed_travel: float = 3000.0

    # 坐标系
    # 部分机器 Y+ 朝上（在纸上向下走对应 -Y），开启后输出 y = -y。
    invert_y: bool = False

    # 输出
    decimals: int = 3
    header: tuple[str, ...] = DEFAULT_HEADER
    footer: tuple[str, ...] = ("M5", "G0 X0 Y0")

    # 分段
    min_segment_mm: float = 0.05
    max_segment_mm: float = 1.0

    # 单点笔画（点）的驻留时间，秒。不留驻留时间纸面留不下墨点
    dot_dwell_s: float = 0.05


@dataclass(slots=True)
class PlotStats:
    """生成结果的统计信息，用于评估可执行性。"""

    stroke_count: int = 0
    point_count: int = 0
    travel_count: int = 0
    draw_length_mm: float = 0.0
    travel_length_mm: float = 0.0
    line_count: int = 0
    estimated_seconds: float = 0.0
    lines: list[str] = field(default_factory=list)


def _fmt(value: float, decimals: int) -> str:
    text = f"{value:.{decimals}f}"
    return text.rstrip("0").rstrip(".") if "." in text else text


class _Writer:
    def __init__(self, settings: PlotSettings) -> None:
        self.s = settings
        self.lines: list[str] = []
        self.stroke_count = 0
        self.point_count = 0
        self.travel_count = 0
        self.draw_length = 0.0
        self.travel_length = 0.0
        self._pen_down = False

    # -- 基础动作 --------------------------------------------------------
    def raw(self, line: str) -> None:
        self.lines.append(line)

    def pen_up(self) -> None:
        if not self._pen_down:
            return
        self._pen_down = False
        if self.s.use_z_axis:
            self.lines.append(f"G0 Z{_fmt(self.s.pen_up_z, self.s.decimals)}")
        else:
            self.lines.append(self.s.pen_up_cmd)

    def pen_down(self) -> None:
        if self._pen_down:
            return
        self._pen_down = True
        if self.s.use_z_axis:
            self.lines.append(f"G0 Z{_fmt(self.s.pen_down_z, self.s.decimals)}")
        else:
            self.lines.append(self.s.pen_down_cmd)

    def travel_to(self, x: float, y: float) -> None:
        self.pen_up()
        out_y = -y if self.s.invert_y else y
        self.lines.append(f"G0 X{_fmt(x, self.s.decimals)} Y{_fmt(out_y, self.s.decimals)}")

    def draw_to(self, x: float, y: float) -> None:
        self.pen_down()
        out_y = -y if self.s.invert_y else y
        self.lines.append(
            f"G1 X{_fmt(x, self.s.decimals)} Y{_fmt(out_y, self.s.decimals)} "
            f"F{_fmt(self.s.feed_draw, 0)}"
        )

    # -- 笔画 ------------------------------------------------------------
    def plot_stroke(
        self, points: Sequence[Point], origin: tuple[float, float], size: float
    ) -> None:
        if not points:
            return
        ox, oy = origin
        self.stroke_count += 1
        self.point_count += len(points)

        first = points[0]
        first_x, first_y = ox + first.x * size, oy + first.y * size
        self.travel_to(first_x, first_y)
        self.travel_length += _distance(ox, oy, first_x, first_y)

        if len(points) == 1:
            # 单点笔画：落笔后驻留一小段时间，再抬笔，在纸面留下一个墨点
            self.pen_down()
            self.lines.append(f"G4 P{self.s.dot_dwell_s:.2f}")
            self.pen_up()
            return

        prev_x, prev_y = first_x, first_y
        for point in points[1:]:
            x, y = ox + point.x * size, oy + point.y * size
            self.draw_length += _distance(prev_x, prev_y, x, y)
            self.draw_to(x, y)
            prev_x, prev_y = x, y

        self.pen_up()

    def finish(self) -> PlotStats:
        self.pen_up()
        for line in self.s.footer:
            # 跳过与上一行重复的页脚指令：主轴模式下抬笔本身就是 M5，
            # 页脚再写一次会输出两个连续的 M5（无害但脏）
            if not self.lines or self.lines[-1] != line:
                self.lines.append(line)
        return PlotStats(
            stroke_count=self.stroke_count,
            point_count=self.point_count,
            travel_count=self.travel_count,
            draw_length_mm=self.draw_length,
            travel_length_mm=self.travel_length,
            line_count=len(self.lines),
            estimated_seconds=self.draw_length / (self.s.feed_draw / 60.0)
            + self.travel_length / (self.s.feed_travel / 60.0),
            lines=self.lines,
        )


def _distance(x1: float, y1: float, x2: float, y2: float) -> float:
    return ((x2 - x1) ** 2 + (y2 - y1) ** 2) ** 0.5


def char_origin(row: int, col: int, settings: PlotSettings) -> tuple[float, float]:
    """字格左上角在机床坐标系中的位置（mm）。

    G 代码生成与 SVG 预览共用这一个函数。若两边各自算一遍，
    预览图就会与机器实际画出来的位置不一致 —— 而预览的全部意义
    就是「不动机器也能看到机器会画成什么样」。
    """
    return (
        settings.margin_x_mm + col * (settings.char_size_mm + settings.char_gap_mm),
        settings.margin_y_mm + row * (settings.char_size_mm + settings.line_gap_mm),
    )


def page_size(row_count: int, settings: PlotSettings) -> tuple[float, float]:
    """整页幅面尺寸（mm）。"""
    cols = max(1, settings.chars_per_line)
    rows = max(1, row_count)
    width = (
        settings.margin_x_mm * 2 + cols * settings.char_size_mm + (cols - 1) * settings.char_gap_mm
    )
    height = (
        settings.margin_y_mm * 2 + rows * settings.char_size_mm + (rows - 1) * settings.line_gap_mm
    )
    return width, height


def strokes_to_gcode(
    rows: Iterable[Sequence[StrokeSet]],
    settings: PlotSettings | None = None,
) -> str:
    """把按行分组的字形轨迹转换成 GRBL G 代码文本。

    ``rows`` 的每个元素是一行，行内每个元素是一个字。
    """
    return strokes_to_gcode_with_stats(rows, settings).lines_text()


def strokes_to_gcode_with_stats(
    rows: Iterable[Sequence[StrokeSet]],
    settings: PlotSettings | None = None,
) -> _GcodeResult:
    """按行分组生成。保留此入口是为了兼容「行内固定字距」的通用排版。"""
    s = settings or PlotSettings()
    writer = _Writer(s)
    for line in s.header:
        writer.raw(line)

    for row_index, row in enumerate(rows):
        for col_index, stroke_set in enumerate(row):
            origin_x, origin_y = char_origin(row_index, col_index, s)
            for stroke in stroke_set.strokes:
                writer.plot_stroke(stroke.points, (origin_x, origin_y), s.char_size_mm)

    stats = writer.finish()
    return _GcodeResult(stats)


def glyphs_to_gcode_with_stats(
    glyphs: Sequence[StrokeSet],
    page: Page,
    settings: PlotSettings | None = None,
) -> _GcodeResult:
    """按版面字格生成 G 代码。

    这是正式入口：字格位置与字宽由 :mod:`handwriting.layout` 统一计算，
    本函数只负责把字形摆进字格并翻译成 G 代码。这样预览画出来的位置
    与机器实际走的位置必然一致。

    与 C++ 侧 ``handwriting::gcode::glyphs_to_gcode`` 行为等价。
    """
    s = settings or PlotSettings()
    writer = _Writer(s)
    for line in s.header:
        writer.raw(line)

    for glyph, cell in zip(glyphs, page.cells, strict=False):
        for stroke in glyph.strokes:
            writer.plot_stroke(stroke.points, (cell.x_mm, cell.y_mm), cell.size_mm)

    stats = writer.finish()
    return _GcodeResult(stats)


@dataclass(slots=True)
class _GcodeResult:
    stats: PlotStats

    def lines_text(self) -> str:
        return "\n".join(self.stats.lines) + "\n"
