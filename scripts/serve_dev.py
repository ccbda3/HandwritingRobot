"""开发者模式：左边粘贴文章，右边实时显示模型推理 + 排版。

只用 Python 标准库，不引入 Web 框架：

    python scripts/serve_dev.py --model data/exports/style-model.pt

然后打开 http://127.0.0.1:8765

页面把文本 POST 到 ``/render``，服务端用字形骨架渲染 → 风格模型推理 →
按版面排版 → 返回一张 SVG。长文会把所有笔画摊平成一批做前向，避免逐字调用。
"""

from __future__ import annotations

import argparse
import contextlib
import json
import math
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from handwriting.glyph import HanziWriterSource
from handwriting.layout import LayoutSettings, Mode, compute
from handwriting.model.generate import stylize_stroke_sets
from handwriting.model.train import load_checkpoint
from handwriting.preview import SvgStyle, to_svg
from handwriting.render import RenderSettings, render_text

MODEL = None
CONFIG = None
SOURCE: HanziWriterSource | None = None
LOCK = threading.Lock()
UI = ""


def render_page(
    text: str,
    mode: str,
    char_size: float,
    chars_per_line: int,
    margin: float,
    strength: float,
) -> dict:
    started = time.time()
    if not text.strip():
        return {
            "svg": "",
            "chars": 0,
            "lines": 0,
            "missing": [],
            "width": 0,
            "height": 0,
            "elapsed": 0,
        }

    result = render_text(text, source=SOURCE, settings=RenderSettings(), chars_per_line=0)
    if result.is_empty:
        return {
            "svg": "",
            "chars": 0,
            "lines": 0,
            "missing": result.missing,
            "width": 0,
            "height": 0,
            "elapsed": round((time.time() - started) * 1000),
        }

    flat = [glyph for row in result.rows for glyph in row]
    with LOCK:
        styled = stylize_stroke_sets(MODEL, CONFIG, flat, strength=strength)

    rows = []
    index = 0
    for row in result.rows:
        rows.append(styled[index : index + len(row)])
        index += len(row)

    per_line = max(1, chars_per_line)
    wrapped_lines = sum(max(1, math.ceil(len(row) / per_line)) for row in rows)

    layout = LayoutSettings(
        mode=Mode.NOTEBOOK if mode == "notebook" else Mode.GRID,
        char_size_mm=max(4.0, char_size),
        chars_per_line=max(1, chars_per_line),
        margin_x_mm=max(0.0, margin),
        margin_y_mm=max(0.0, margin),
        char_gap_mm=2.0,
        line_gap_mm=6.0,
    )
    page = compute([len(row) for row in rows], layout)
    glyphs = [glyph for row in rows for glyph in row]
    svg = to_svg(glyphs, page, SvgStyle(show_grid=False, show_rules=False))

    return {
        "svg": svg,
        "chars": len(glyphs),
        "lines": wrapped_lines,
        "missing": result.missing,
        "width": round(page.width_mm, 1),
        "height": round(page.height_mm, 1),
        "elapsed": round((time.time() - started) * 1000),
    }


class Handler(BaseHTTPRequestHandler):
    def do_GET(self) -> None:
        if self.path not in ("/", "/index.html"):
            self.send_error(404)
            return
        data = UI.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self) -> None:
        if self.path != "/render":
            self.send_error(404)
            return
        try:
            length = int(self.headers.get("Content-Length", 0))
            body = json.loads(self.rfile.read(length) or b"{}")
            payload = render_page(
                text=str(body.get("text", "")),
                mode=str(body.get("mode", "grid")),
                char_size=float(body.get("char_size", 10.0)),
                chars_per_line=int(body.get("chars_per_line", 24)),
                margin=float(body.get("margin", 8.0)),
                strength=float(body.get("strength", 1.0)),
            )
            data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
            self.send_response(200)
        except Exception as exc:
            data = json.dumps({"error": str(exc)}, ensure_ascii=False).encode("utf-8")
            self.send_response(500)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, format: str, *args) -> None:
        return


def main() -> int:
    global MODEL, CONFIG, SOURCE, UI

    parser = argparse.ArgumentParser(description="开发者模式：文章 → 模型排版")
    parser.add_argument("--model", type=Path, default=Path("data/exports/style-model.pt"))
    parser.add_argument("--cache-dir", type=Path, default=Path(".cache/glyphs"))
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--offline", action="store_true", help="只用本地字形缓存")
    args = parser.parse_args()

    if not args.model.is_file():
        print(f"找不到模型：{args.model}")
        print('先运行：handwriting train --chars "..." --offline')
        return 1

    MODEL, CONFIG, _meta = load_checkpoint(args.model, "cpu")
    SOURCE = HanziWriterSource(cache_dir=args.cache_dir, offline=args.offline)
    UI = Path(__file__).with_name("dev_ui.html").read_text(encoding="utf-8")

    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"开发者模式已启动：http://127.0.0.1:{args.port}")
    print("按 Ctrl+C 停止")
    with contextlib.suppress(KeyboardInterrupt):
        server.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
