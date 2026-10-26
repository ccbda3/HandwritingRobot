"""开发者模式：左边粘贴文章，右边实时显示模型推理 + 排版，并可直接点字编辑。

推理默认走 C++（``hr-style-batch``），符合 ADR-0007；找不到二进制时回退 Python
参考实现，方便还没编译核心的机器。

    python scripts/serve_dev.py --model data/exports/style-model.pt \
        --weights data/exports/style-model.json

然后打开 http://127.0.0.1:8765

``/render`` 返回：

- ``svg``：排版后的图
- ``wrapped``：按字数折行后的文本行
- ``cells``：每个字格的毫米坐标（前端用来把光标对到格子上）
- ``page``：页面宽高（毫米）
"""

from __future__ import annotations

import argparse
import contextlib
import json
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse

from handwriting.glyph import HanziWriterSource
from handwriting.layout import LayoutSettings, Mode, compute
from handwriting.model.generate import stylize_stroke_sets
from handwriting.model.train import load_checkpoint
from handwriting.preview import SvgStyle, to_svg
from handwriting.stroke import Point, Stroke, StrokeSet

MODEL = None
CONFIG = None
MODEL_PATH: Path | None = None
WEIGHTS: Path | None = None
STYLE_BATCH: Path | None = None
INFER = "auto"
SOURCE: HanziWriterSource | None = None
LOCK = threading.Lock()
UI = ""


def _ensure_python_model():
    global MODEL, CONFIG
    if MODEL is None or CONFIG is None:
        if MODEL_PATH is None or not MODEL_PATH.is_file():
            raise RuntimeError(f"找不到 Python 模型：{MODEL_PATH}")
        MODEL, CONFIG, _ = load_checkpoint(MODEL_PATH, "cpu")
    return MODEL, CONFIG


def _find_batch_binary(explicit: Path | None) -> Path | None:
    if explicit is not None:
        return explicit if explicit.is_file() else None
    candidates = [
        Path("build-core/core/hr-style-batch.exe"),
        Path("build-core/core/hr-style-batch"),
        Path("build/core/hr-style-batch.exe"),
        Path("build/core/hr-style-batch"),
    ]
    return next((path for path in candidates if path.is_file()), None)


def _pack_sets(stroke_sets: list[StrokeSet]) -> list[dict]:
    return [
        {
            "char": stroke_set.char,
            "strokes": [
                [[p.x, p.y, p.pressure, p.t] for p in stroke.points]
                for stroke in stroke_set.strokes
            ],
        }
        for stroke_set in stroke_sets
    ]


def _unpack_sets(payload: dict) -> list[StrokeSet]:
    result: list[StrokeSet] = []
    for item in payload.get("sets", []):
        strokes = []
        for stroke in item.get("strokes", []):
            points = [
                Point(float(x), float(y), float(pressure), int(t)) for x, y, pressure, t in stroke
            ]
            strokes.append(Stroke(points))
        result.append(StrokeSet(strokes, str(item.get("char", ""))))
    return result


def _stylize(stroke_sets: list[StrokeSet], strength: float) -> tuple[list[StrokeSet], str]:
    """优先 C++ 批量推理；失败或未编译时回退 Python 参考实现。"""
    if INFER in ("auto", "cpp") and STYLE_BATCH is not None and WEIGHTS is not None:
        try:
            payload = {"strength": strength, "sets": _pack_sets(stroke_sets)}
            proc = subprocess.run(
                [str(STYLE_BATCH), "--model", str(WEIGHTS)],
                input=json.dumps(payload, ensure_ascii=False).encode("utf-8"),
                capture_output=True,
                timeout=120,
                check=False,
            )
            if proc.returncode != 0:
                raise RuntimeError(
                    proc.stderr.decode("utf-8", "replace")[:300] or "hr-style-batch 失败"
                )
            return _unpack_sets(json.loads(proc.stdout.decode("utf-8"))), "cpp"
        except Exception as exc:
            if INFER == "cpp":
                raise
            print(f"[warn] C++ 推理失败，回退 Python：{exc}")

    model, config = _ensure_python_model()
    with LOCK:
        styled = stylize_stroke_sets(model, config, stroke_sets, strength=strength)
    return styled, "python"


def _wrap_text(text: str, per_line: int) -> list[str]:
    """按字数折行；去掉所有空白（空白不占字格）。"""
    lines: list[str] = []
    normalized = text.replace("\r\n", "\n").replace("\r", "\n")
    for paragraph in normalized.split("\n"):
        paragraph = "".join(paragraph.split())
        if not paragraph:
            continue
        for start in range(0, len(paragraph), per_line):
            lines.append(paragraph[start : start + per_line])
    return lines


def _empty_payload(elapsed: int = 0) -> dict:
    return {
        "svg": "",
        "chars": 0,
        "lines": 0,
        "missing": [],
        "width": 0,
        "height": 0,
        "elapsed": elapsed,
        "cells": [],
        "wrapped": [],
        "page": {"width": 0, "height": 0},
        "backend": "none",
    }


def render_page(
    text: str,
    mode: str,
    char_size: float,
    chars_per_line: int,
    margin: float,
    strength: float,
) -> dict:
    started = time.time()
    per_line = max(1, chars_per_line)
    lines = _wrap_text(text, per_line)
    if not lines:
        return _empty_payload()

    flat = [char for line in lines for char in line]
    glyphs: list[StrokeSet | None] = []
    missing: list[str] = []
    for char in flat:
        try:
            glyph = SOURCE.load(char)  # type: ignore[union-attr]
        except Exception:
            glyphs.append(None)
            if char not in missing:
                missing.append(char)
            continue
        glyphs.append(glyph.to_stroke_set())

    indices = [index for index, glyph in enumerate(glyphs) if glyph is not None]
    backend = "none"
    if indices:
        styled, backend = _stylize([glyphs[index] for index in indices], strength)
        for index, stroke_set in zip(indices, styled, strict=True):
            glyphs[index] = stroke_set

    final = [
        glyph if glyph is not None else StrokeSet([], char)
        for glyph, char in zip(glyphs, flat, strict=True)
    ]

    layout = LayoutSettings(
        mode=Mode.NOTEBOOK if mode == "notebook" else Mode.GRID,
        char_size_mm=max(4.0, char_size),
        chars_per_line=per_line,
        margin_x_mm=max(0.0, margin),
        margin_y_mm=max(0.0, margin),
        char_gap_mm=2.0,
        line_gap_mm=6.0,
    )
    page = compute([len(line) for line in lines], layout)
    svg = to_svg(final, page, SvgStyle(show_grid=False, show_rules=False))

    cells = []
    for index, cell in enumerate(page.cells):
        char = flat[index] if index < len(flat) else ""
        cells.append(
            {
                "char": char,
                "x": round(cell.x_mm, 3),
                "y": round(cell.y_mm, 3),
                "size": round(cell.size_mm, 3),
                "line": cell.line,
                "column": cell.column,
                "missing": char in missing,
            }
        )

    return {
        "svg": svg,
        "chars": len(flat),
        "lines": len(lines),
        "missing": missing,
        "width": round(page.width_mm, 1),
        "height": round(page.height_mm, 1),
        "elapsed": round((time.time() - started) * 1000),
        "cells": cells,
        "wrapped": lines,
        "page": {
            "width": round(page.width_mm, 3),
            "height": round(page.height_mm, 3),
        },
        "backend": backend,
    }


class Handler(BaseHTTPRequestHandler):
    def do_GET(self) -> None:
        if urlparse(self.path).path not in ("/", "/index.html"):
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
                chars_per_line=int(body.get("chars_per_line", 20)),
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
    global MODEL_PATH, WEIGHTS, STYLE_BATCH, INFER, SOURCE, UI

    parser = argparse.ArgumentParser(description="开发者模式：文章 → 模型排版")
    parser.add_argument("--model", type=Path, default=Path("data/exports/style-model.pt"))
    parser.add_argument("--weights", type=Path, default=Path("data/exports/style-model.json"))
    parser.add_argument("--style-batch", type=Path, default=None)
    parser.add_argument("--infer", choices=["auto", "cpp", "python"], default="auto")
    parser.add_argument("--cache-dir", type=Path, default=Path(".cache/glyphs"))
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--offline", action="store_true")
    args = parser.parse_args()

    MODEL_PATH = args.model
    WEIGHTS = args.weights
    STYLE_BATCH = _find_batch_binary(args.style_batch)
    INFER = args.infer

    if INFER == "auto":
        INFER = "cpp" if STYLE_BATCH is not None and WEIGHTS.is_file() else "python"
    if INFER == "cpp" and (STYLE_BATCH is None or not WEIGHTS.is_file()):
        print("错误：--infer cpp 需要 hr-style-batch 与权重 JSON")
        return 1
    if INFER == "python" and not MODEL_PATH.is_file():
        print(f"错误：找不到 Python 模型 {MODEL_PATH}")
        return 1

    SOURCE = HanziWriterSource(cache_dir=args.cache_dir, offline=args.offline)
    UI = Path(__file__).with_name("dev_ui.html").read_text(encoding="utf-8")

    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"开发者模式已启动：http://127.0.0.1:{args.port}")
    print(f"推理后端：{INFER}" + (f"（{STYLE_BATCH}）" if INFER == "cpp" else ""))
    print("按 Ctrl+C 停止")
    with contextlib.suppress(KeyboardInterrupt):
        server.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
