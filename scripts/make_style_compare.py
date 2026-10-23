"""生成「标准骨架 vs 模型生成」的交互对比页。

用法::

    python scripts/make_style_compare.py \
        --model data/exports/style-model.pt \
        --out data/exports/style-compare.html

页面功能：切换字符、笔画演示（逐笔动画）、速度调节；
加 ``?static=1`` 可直接看画完的静态对比。
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from handwriting.glyph import HanziWriterSource
from handwriting.model.generate import stylize_stroke_set
from handwriting.model.train import load_checkpoint
from handwriting.render import render_text


def _pack(stroke_set) -> list:
    return [
        [[round(p.x, 4), round(p.y, 4), round(p.pressure, 3)] for p in stroke.points]
        for stroke in stroke_set.strokes
    ]


def _cached_chars(cache_dir: Path) -> str:
    return "".join(chr(int(path.stem[2:], 16)) for path in sorted(cache_dir.glob("U+*.json")))


TEMPLATE = r"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<title>风格模型 · 学习效果对比</title>
<style>
  :root{--bg:#0f1115;--panel:#171a21;--line:#2a2f3a;--text:#e6e6e6;--dim:#9aa0a6;--accent:#67d4ff;--red:#e5484d}
  *{box-sizing:border-box}
  body{margin:0;padding:24px;background:var(--bg);color:var(--text);font-family:-apple-system,"Segoe UI","Microsoft YaHei",sans-serif}
  header{display:flex;align-items:baseline;gap:12px;margin-bottom:14px;flex-wrap:wrap}
  h1{font-size:18px;font-weight:500;margin:0}
  .sub{color:var(--dim);font-size:13px}
  .chars{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:16px}
  .chars button{background:var(--panel);color:var(--text);border:1px solid var(--line);border-radius:8px;padding:7px 13px;font-size:15px;cursor:pointer}
  .chars button:hover{border-color:#3a4150}
  .chars button.active{border-color:var(--accent);color:var(--accent);background:#132532}
  .stage{display:flex;gap:20px;flex-wrap:wrap}
  .card{background:#fff;border-radius:12px;padding:10px}
  .card h2{font-size:13px;font-weight:500;color:#444;text-align:center;margin:2px 0 8px}
  canvas{display:block;width:360px;height:360px;border-radius:8px}
  .controls{display:flex;gap:12px;align-items:center;margin-top:16px;flex-wrap:wrap}
  button.primary{background:#12303f;color:var(--accent);border:1px solid #1f5a73;border-radius:8px;padding:9px 18px;font-size:14px;cursor:pointer}
  button.primary:hover{background:#164054}
  button.ghost{background:var(--panel);color:var(--text);border:1px solid var(--line);border-radius:8px;padding:9px 16px;font-size:14px;cursor:pointer}
  label{color:var(--dim);font-size:13px;display:flex;align-items:center;gap:8px}
  input[type=range]{accent-color:var(--accent);width:120px}
  .note{margin-top:14px;color:var(--dim);font-size:12px;line-height:1.6}
  .pen{display:inline-block;width:9px;height:9px;border-radius:50%;background:var(--red);margin-right:6px;vertical-align:middle}
</style>
</head>
<body>
<header>
  <h1>风格模型 · 学习效果</h1>
  <span class="sub">标准骨架 vs 模型生成（合成风格，非个人笔迹）</span>
</header>
<div class="chars" id="chars"></div>
<div class="stage">
  <div class="card"><h2>标准骨架</h2><canvas id="cvA" width="720" height="720"></canvas></div>
  <div class="card"><h2>模型生成</h2><canvas id="cvB" width="720" height="720"></canvas></div>
</div>
<div class="controls">
  <button id="play" class="primary">▶ 笔画演示</button>
  <button id="reset" class="ghost">重置</button>
  <label>速度 <input id="speed" type="range" min="0.3" max="3" step="0.1" value="1"></label>
  <span id="status" class="sub"></span>
</div>
<div class="note"><span class="pen"></span>红点是当前笔尖位置。演示字形来自训练集（13 个），用于观察模型是否学到倾斜、提按与抖动；真实个人风格需要数位板数据。</div>
<script>
const DATA = __DATA__;
const chars = Object.keys(DATA);
const SIZE = 720, M = 78, INNER = SIZE - 2 * M;
const cvA = document.getElementById('cvA'), cvB = document.getElementById('cvB');
const ctxA = cvA.getContext('2d'), ctxB = cvB.getContext('2d');
let current = chars[0], frac = 0, playing = false, lastT = 0, raf = null;
let totalA = 0, totalB = 0;

function countPoints(strokes){ let n = 0; for(const s of strokes) n += s.length; return n; }

function setup(){
  const d = DATA[current];
  totalA = countPoints(d.original);
  totalB = countPoints(d.stylized);
  frac = 0;
  render();
}

function drawSide(ctx, strokes, prog){
  ctx.clearRect(0, 0, SIZE, SIZE);
  ctx.fillStyle = '#ffffff'; ctx.fillRect(0, 0, SIZE, SIZE);
  ctx.strokeStyle = '#e6e6e6'; ctx.lineWidth = 2;
  ctx.strokeRect(M * 0.62, M * 0.62, SIZE - M * 1.24, SIZE - M * 1.24);
  ctx.strokeStyle = '#f1f1f1'; ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(SIZE/2, M*0.62); ctx.lineTo(SIZE/2, SIZE-M*0.62);
  ctx.moveTo(M*0.62, SIZE/2); ctx.lineTo(SIZE-M*0.62, SIZE/2);
  ctx.stroke();

  let remain = prog, pen = null;
  for(const stroke of strokes){
    if(remain <= 0) break;
    const take = Math.min(stroke.length, remain);
    if(take >= 2){
      for(let i = 1; i < take; i++){
        const a = stroke[i-1], b = stroke[i];
        ctx.beginPath();
        ctx.strokeStyle = '#141414';
        ctx.lineWidth = 2 + 7 * ((a[2] + b[2]) / 2);
        ctx.lineCap = 'round'; ctx.lineJoin = 'round';
        ctx.moveTo(M + a[0] * INNER, M + a[1] * INNER);
        ctx.lineTo(M + b[0] * INNER, M + b[1] * INNER);
        ctx.stroke();
      }
      const last = stroke[take-1];
      pen = [M + last[0] * INNER, M + last[1] * INNER];
    } else if(take === 1){
      const p = stroke[0];
      ctx.beginPath(); ctx.fillStyle = '#141414';
      ctx.arc(M + p[0] * INNER, M + p[1] * INNER, 4 + 5 * p[2], 0, Math.PI * 2);
      ctx.fill();
      pen = [M + p[0] * INNER, M + p[1] * INNER];
    }
    remain -= take;
  }
  if(pen && frac < 0.999){
    ctx.beginPath(); ctx.fillStyle = '#e5484d';
    ctx.arc(pen[0], pen[1], 8, 0, Math.PI * 2); ctx.fill();
    ctx.beginPath(); ctx.strokeStyle = 'rgba(229,72,77,.35)'; ctx.lineWidth = 5;
    ctx.arc(pen[0], pen[1], 15, 0, Math.PI * 2); ctx.stroke();
  }
}

function render(){
  const d = DATA[current];
  drawSide(ctxA, d.original, frac * totalA);
  drawSide(ctxB, d.stylized, frac * totalB);
  const pct = Math.round(frac * 100);
  document.getElementById('status').textContent = '笔画 ' + d.original.length + ' · 进度 ' + pct + '%';
}

function updateButton(){
  document.getElementById('play').textContent = playing ? '❚❚ 暂停' : (frac >= 1 ? '↻ 重播' : '▶ 笔画演示');
}

function tick(t){
  if(!playing) return;
  if(!lastT) lastT = t;
  const dt = (t - lastT) / 1000; lastT = t;
  const speed = parseFloat(document.getElementById('speed').value);
  frac += dt * speed / 3.2;
  if(frac >= 1){ frac = 1; playing = false; }
  render();
  if(playing) raf = requestAnimationFrame(tick); else updateButton();
}

const charsBox = document.getElementById('chars');
chars.forEach(ch => {
  const b = document.createElement('button');
  b.textContent = ch;
  b.dataset.ch = ch;
  b.onclick = () => {
    current = ch; playing = false;
    document.querySelectorAll('.chars button').forEach(x => x.classList.toggle('active', x.dataset.ch === ch));
    setup(); updateButton();
  };
  charsBox.appendChild(b);
});
document.querySelector('.chars button').classList.add('active');

document.getElementById('play').onclick = () => {
  if(playing){ playing = false; updateButton(); return; }
  if(frac >= 1) frac = 0;
  playing = true; lastT = 0; updateButton();
  raf = requestAnimationFrame(tick);
};
document.getElementById('reset').onclick = () => {
  playing = false; if(raf) cancelAnimationFrame(raf);
  frac = 0; render(); updateButton();
};

setup();
const _params = new URLSearchParams(location.search);
if(_params.get('static') === '1'){ frac = 1; render(); }
updateButton();
if(_params.get('play') === '1'){ document.getElementById('play').click(); }
</script>
</body>
</html>"""


def main() -> int:
    parser = argparse.ArgumentParser(description="生成风格模型对比页")
    parser.add_argument("--model", type=Path, default=Path("data/exports/style-model.pt"))
    parser.add_argument("--out", type=Path, default=Path("data/exports/style-compare.html"))
    parser.add_argument("--chars", default=None, help="要展示的字；默认取字形缓存里的全部")
    parser.add_argument("--cache-dir", type=Path, default=Path(".cache/glyphs"))
    parser.add_argument("--device", default="cpu")
    args = parser.parse_args()

    model, config, _meta = load_checkpoint(args.model, args.device)
    source = HanziWriterSource(cache_dir=args.cache_dir, offline=True)
    chars = args.chars or _cached_chars(args.cache_dir)

    data: dict[str, dict] = {}
    for char in chars:
        result = render_text(char, source=source, chars_per_line=0)
        if result.is_empty:
            continue
        original = result.rows[0][0]
        stylized = stylize_stroke_set(model, config, original)
        data[char] = {"original": _pack(original), "stylized": _pack(stylized)}

    if not data:
        print("没有可展示的字形，请检查字形缓存")
        return 1

    payload = json.dumps(data, ensure_ascii=False, separators=(",", ":"))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(TEMPLATE.replace("__DATA__", payload), encoding="utf-8")
    print(f"已生成 {args.out}（{len(data)} 个字，{len(payload)} 字节）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
