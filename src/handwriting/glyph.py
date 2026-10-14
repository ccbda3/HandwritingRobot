"""字形骨架：从标准笔顺库获取笔画中心线。

## 数据源

`hanzi-writer-data <https://github.com/chanind/hanzi-writer-data>`_（派生自
`Make Me a Hanzi <https://github.com/skishore/makemeahanzi>`_，最终来源是文鼎 PL 字体）。
每个字符一个 JSON，含两个关键字段：

``strokes``
    SVG 轮廓路径 —— **本模块不用**。轮廓只有形状，没有先后顺序。
``medians``
    笔画中心线折线 —— 本模块只用这个。

**medians 自带正确的笔顺**：数组顺序就是书写顺序，第 0 笔、第 1 笔……
这正是本项目相对「描图片轮廓」方案的核心区别。

## 坐标系

Make Me a Hanzi 用 1024×1024 的 em 框，**原点在基线、y 轴向上**。
本模块统一转换到与采集器一致的坐标系：原点在方格左上角、y 轴向下、取值 0~1。

em 框取 ``x ∈ [0, 1024]``、``y ∈ [-124, 900]``。这个范围不是拍脑袋定的，
用实际数据验证过：

- 「口」落在方格约 22%~74% 处 —— 小字应当居中偏小，符合字格观感
- 「一」落在 45%~52% 高度 —— 横画应当在中线略上方
- 「国」「人」接近撑满 —— 大字应当占满字格

若改用「逐字包围盒归一化」，上面三条全部失效：「一」会被拉伸成一条贯穿
整格的横线，而「口」会被放大到与「国」一样大。

## 许可

数据派生自 Arphic PL 字体，**不随仓库分发**。本模块按需下载并缓存到本地，
缓存目录已在 ``.gitignore`` 中排除。详见
[ADR-0004](../../docs/adr/0004-license-and-third-party-data.md)。
"""

from __future__ import annotations

import json
import time
import urllib.error
import urllib.parse
import urllib.request
from collections.abc import Iterable, Mapping
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Protocol

from handwriting import curve
from handwriting.stroke import Point, Stroke, StrokeSet

EM_MIN_X = 0.0
EM_MIN_Y = -124.0
EM_MAX_X = 1024.0
EM_MAX_Y = 900.0
EM_SIZE = EM_MAX_X - EM_MIN_X

DEFAULT_BASE_URL = "https://cdn.jsdelivr.net/npm/hanzi-writer-data@2.0.1/"
DEFAULT_CACHE_DIR = Path(".cache") / "glyphs"

DEFAULT_RESAMPLE_STEP = 0.01
DEFAULT_SAMPLES_PER_SEGMENT = 12


class GlyphNotFoundError(LookupError):
    """字形数据源里没有这个字符，或本地缓存缺失且处于离线模式。"""


@dataclass(frozen=True, slots=True)
class Glyph:
    """一个字符的笔画中心线，坐标仍为 Make Me a Hanzi 原始单位。"""

    char: str
    medians: tuple[tuple[tuple[float, float], ...], ...]

    @property
    def stroke_count(self) -> int:
        return len(self.medians)

    @property
    def point_count(self) -> int:
        return sum(len(median) for median in self.medians)

    def bounds(self) -> tuple[float, float, float, float]:
        """原始单位下的包围盒 ``(min_x, min_y, max_x, max_y)``。"""
        xs = [x for median in self.medians for x, _ in median]
        ys = [y for median in self.medians for _, y in median]
        if not xs:
            raise ValueError(f"{self.char!r} 没有笔画中心线")
        return min(xs), min(ys), max(xs), max(ys)

    def to_stroke_set(
        self,
        step: float = DEFAULT_RESAMPLE_STEP,
        samples_per_segment: int = DEFAULT_SAMPLES_PER_SEGMENT,
        smooth: bool = True,
    ) -> StrokeSet:
        """转换到归一化坐标系（0~1，y 向下），并按弧长重采样。

        默认先做 Catmull-Rom 样条插值：medians 是稀疏折线
        （例如「永」的第一笔只有 4 个点），直接连线会出现肉眼可见的折角。
        """
        strokes: list[Stroke] = []
        for median in self.medians:
            if not median:
                continue
            points = [
                Point(x=(x - EM_MIN_X) / EM_SIZE, y=(EM_MAX_Y - y) / EM_SIZE, pressure=0.5, t=0)
                for x, y in median
            ]
            if smooth and len(points) >= 2:
                points = curve.smooth_and_resample(points, step, samples_per_segment)
            strokes.append(Stroke(points))
        return StrokeSet(strokes, self.char)


class GlyphSource(Protocol):
    """字形数据源接口。实现方只需提供 :meth:`load`。"""

    def load(self, char: str) -> Glyph:  # pragma: no cover - 协议声明
        ...


def parse_glyph(char: str, payload: Mapping[str, Any]) -> Glyph:
    """从 hanzi-writer 格式的 JSON 构造 :class:`Glyph`。"""
    raw = payload.get("medians")
    if not isinstance(raw, list):
        raise ValueError(f"{char!r} 的字形数据缺少 medians 字段")

    medians: list[tuple[tuple[float, float], ...]] = []
    for index, median in enumerate(raw):
        if not isinstance(median, list) or not median:
            continue
        try:
            points = tuple((float(p[0]), float(p[1])) for p in median)
        except (TypeError, IndexError, ValueError) as exc:
            raise ValueError(f"{char!r} 第 {index} 笔的中心线格式非法") from exc
        medians.append(points)

    if not medians:
        raise ValueError(f"{char!r} 没有任何有效的笔画中心线")
    return Glyph(char=char, medians=tuple(medians))


class HanziWriterSource:
    """按需下载字形并缓存到本地。

    缓存文件名用 Unicode 码位（``U+6C38.json``）而不是字符本身：
    Windows 上非 ASCII 文件名容易踩编码坑，而码位名永远是纯 ASCII。
    """

    def __init__(
        self,
        cache_dir: str | Path = DEFAULT_CACHE_DIR,
        base_url: str = DEFAULT_BASE_URL,
        timeout: float = 30.0,
        offline: bool = False,
        retries: int = 3,
        retry_delay: float = 0.6,
        request_interval: float = 0.05,
    ) -> None:
        if retries < 1:
            raise ValueError(f"retries 必须 ≥ 1，收到 {retries}")
        self.cache_dir = Path(cache_dir)
        self.base_url = base_url if base_url.endswith("/") else base_url + "/"
        self.timeout = timeout
        self.offline = offline
        self.retries = retries
        self.retry_delay = retry_delay
        self.request_interval = request_interval

    def cache_path(self, char: str) -> Path:
        return self.cache_dir / f"U+{ord(char):04X}.json"

    def load(self, char: str) -> Glyph:
        if len(char) != 1:
            raise ValueError(f"一次只能加载一个字符，收到 {char!r}")

        cached = self.cache_path(char)
        if cached.is_file():
            return parse_glyph(char, json.loads(cached.read_text(encoding="utf-8")))

        if self.offline:
            raise GlyphNotFoundError(f"离线模式下本地没有 {char!r} 的缓存（{cached}）")

        payload = self._download(char)
        cached.parent.mkdir(parents=True, exist_ok=True)
        cached.write_text(json.dumps(payload, ensure_ascii=False), encoding="utf-8")
        return parse_glyph(char, payload)

    def preload(self, chars: Iterable[str]) -> dict[str, str]:
        """批量预取。返回 ``{字符: 失败原因}``，全部成功时为空字典。

        请求之间插入短暂间隔：预取 150 个字会对 CDN 连发 150 个请求，
        不留间隔容易被限流甚至断连。
        """
        failures: dict[str, str] = {}
        for char in chars:
            try:
                self.load(char)
            except (GlyphNotFoundError, OSError, ValueError) as exc:
                failures[char] = str(exc)
            if self.request_interval > 0.0:
                time.sleep(self.request_interval)
        return failures

    def _download(self, char: str) -> dict[str, Any]:
        """下载单个字形，带指数退避重试。

        必须重试：CDN 偶发断连（``Remote end closed connection``）是常态，
        批量预取 150 个字时几乎必然遇到，不能因此丢掉整个字符。
        但 404 属于永久性失败，重试没有意义，直接抛出。
        """
        url = f"{self.base_url}{urllib.parse.quote(char)}.json"
        last_error: Exception | None = None

        for attempt in range(1, self.retries + 1):
            try:
                with urllib.request.urlopen(url, timeout=self.timeout) as response:
                    return json.load(response)
            except urllib.error.HTTPError as exc:
                if exc.code == 404:
                    raise GlyphNotFoundError(f"字形库中没有 {char!r}") from exc
                last_error = exc
            except (urllib.error.URLError, OSError, json.JSONDecodeError) as exc:
                # OSError 覆盖 RemoteDisconnected / ConnectionResetError / socket.timeout
                last_error = exc

            if attempt < self.retries:
                time.sleep(self.retry_delay * (2 ** (attempt - 1)))

        raise GlyphNotFoundError(
            f"下载 {char!r} 连续失败 {self.retries} 次：{last_error}"
        ) from last_error


def load_glyphs(
    chars: Iterable[str],
    source: GlyphSource | None = None,
) -> tuple[dict[str, Glyph], dict[str, str]]:
    """批量加载，返回 ``(成功字典, 失败字典)``。

    单个字符失败不影响其余字符 —— 生僻字或标点在字形库里可能不存在，
    这不该让整段文本渲染不出来。
    """
    active = source if source is not None else HanziWriterSource()
    loaded: dict[str, Glyph] = {}
    failures: dict[str, str] = {}
    for char in chars:
        try:
            loaded[char] = active.load(char)
        except (GlyphNotFoundError, ValueError, OSError) as exc:
            failures[char] = str(exc)
    return loaded, failures
