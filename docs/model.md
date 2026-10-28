# 笔迹风格模型 v0

> 状态：可训练基线。代码在 `src/handwriting/model/`，需要 `pip install -e ".[train]"`。

## 目标

默认管线输出的是**标准骨架**：结构正确，但不是某个人的字。
风格模型要解决的是：给定任意一条中心线（汉字骨架 / 表格边框 / SVG 路径），
生成「这个人」会怎么写——偏移、倾斜、提按、抖动。

## 形式化

对每一笔：

1. 按弧长重采样到 `N` 个点；
2. 取一条**内容中心线** `c_i`（标准骨架，或合成数据里的基准曲线）；
3. 真实笔迹与中心线之差就是**风格残差** `r_i = p_i - c_i`；
4. 每点特征：

   ```
   x_i = [c_i.x, c_i.y, tangent.x, tangent.y, s, length, r_{i-1}.x, r_{i-1}.y, pressure_{i-1}]
   ```

5. 模型 `f(x_i)` 输出混合高斯（MDN）：`π, μ, σ` 描述 `r_i`，另有一个高斯描述笔压；
6. 损失：残差的混合高斯 **NLL** + `λ` × 笔压的 **NLL**；
7. 生成：`r_i = Σ π_k μ_k`（默认取均值，平滑可复现）或按 `π` 抽样（`--sample`）。

残差在训练时乘以 `RESIDUAL_SCALE`（50）再回归，否则 σ 会塌缩、梯度爆炸。

## 数据从哪来

| 来源 | 中心线 | 说明 |
|---|---|---|
| 合成曲线 | 未加风格的基准曲线 | 没有数位板时自测 |
| 字形骨架 + 合成风格 | 字形骨架 | 更接近真实笔画分布（`--chars`） |
| 真实采集（2.0） | 目前用平滑自身 | 只能学局部抖动；**下一步要做骨架对齐** |

## 命令

```bash
# 合成曲线
handwriting train --samples 300 --epochs 300 -o data/exports/style-model.pt

# 字形骨架 + 合成风格（离线缓存）
handwriting train --chars "不八啼处字春晓永法眠觉闻鸟" --offline \
  -o data/exports/style-model.pt

# 真实采集数据
handwriting train --data capture.json -o data/exports/style-model.pt

# 生成
handwriting stylize "永" --model data/exports/style-model.pt --offline -o out.nc --svg out.svg
handwriting stylize --svg-file drawing.svg --model data/exports/style-model.pt --svg out.svg

# 交互对比页（标准骨架 vs 模型生成，带「笔画演示」动画）
python scripts/make_style_compare.py --model data/exports/style-model.pt
```

## 运行时：推理在 C++

按 [ADR-0007](adr/0007-runtime-in-cpp-python-for-training.md)，产品运行时只用 C++：

```bash
# 训练（Python）
handwriting train --chars "..." --offline -o data/exports/style-model.pt

# 导出给 C++（JSON）
handwriting export-weights --model data/exports/style-model.pt \
  -o data/exports/style-model.json
```

C++ 侧调用 `handwriting::style::Model::load(json).stylize(stroke_set)`，
实现见 `core/src/style_model.cpp`。Python 的 `model/generate.py` 保留为
训练期参考实现；跨语言对拍工具 `hr-style-dump`，当前最大误差 **1.1e-07**。

Qt App 已接入：左栏勾选「使用风格模型」，在「选项 → 风格模型」里选权重 JSON、
调强度；`renderNow` 对每个字调用同一个 `style::Model::stylize`，
预览与导出的 G 代码使用同一份结果。加载失败回退标准骨架，状态栏提示。

## 开发者模式（文章 → 实时推理 + 排版 + 编辑）

```bash
python scripts/serve_dev.py --model data/exports/style-model.pt \
  --weights data/exports/style-model.json
# 打开 http://127.0.0.1:8765
```

- 左侧粘贴文章，右侧实时出模型排版。
- **默认走 C++** `hr-style-batch`（ADR-0007）；找不到二进制才回退 Python。
- 右侧可以直接**点字出光标**编辑：输入替换、方向键移动、Enter 换行、Backspace 删字。
- `/render` 返回每个字格的毫米坐标（`cells`），前端据此把光标对到格子上。
- `?static=1` 只看静态图；`?selftest=1` 跑一次编辑自测。

## 整行风格：三层

要「字的大小、倾斜、飘逸、间隔都像你」，需要三层。当前进度：

| 层 | 内容 | 状态 |
|---|---|---|
| 1. 版面风格 | 字大小、字距、基线抖动/倾斜 | **已实现**（`layout::compute_styled`，Qt「整行风格」） |
| 2. 骨架对齐 | 把采集笔画映射到标准骨架，残差才是你的笔画风格 | **已实现**（`model/align.py`，`train --align`） |
| 3. 序列模型 | 连笔、抬落笔、速度/节奏（飘逸） | **最小骨架已实现**（`model/sequence.py`，合成数据自测；真实训练待数据） |

- 版面层参数目前手动调；真实行数据到位后，可从 `LineStyleFeatures`
  （`char_width_cv` / `char_gap_cv` / `baseline_drift` / `baseline_slope`）自动估计。
- 对齐用法：`handwriting train --data capture.json --align --offline`；
  1.0 单字与 2.0 整行都支持（2.0 需要 `charRanges` 才能切字）。
  默认 `--min-align-cost 0.35` 会跳过匹配代价过高的笔画对；设为 0 关闭过滤。
- 采集入口：App「工具 → 采集与训练…」或网页 `capture/index.html`；
  两者写出的 JSON 格式一致，App 内可直接训练并自动导出 weights。
- 第 3 层骨架：`PenSequenceModel`（GRU + MDN）输入「下一段中心线 + 上一笔尖状态」，
  输出 Δ点 / 抬落笔 / 压感；`train_sequences` / `generate_stroke`。
  目前用合成数据自测，真实训练等行数据；推理最终要按 ADR-0007 落到 C++。

![整行风格：字大小/基线波动，横条本仍压线](images/line-style.png)

## 限制（v0 明确不做的事）

- **不建模笔间抬落笔**：笔画之间固定抬笔，不学连笔。
- **不建模速度/时序**：输出点按弧长均匀，时序由 G 代码进给决定。
- **不重建字形整体比例**：只做相对中心线的局部变形。
- **真实数据尚未训练**：骨架对齐已实现（`train --align`），但还没有数位板行数据，
  当前模型是合成风格。
- 逐点独立预测，`--sample` 时相邻点可能跳到不同混合分量；默认取均值避免跳点。
- 推理已迁到 C++；Python 的 `generate.py` 只作训练期参考，不再进入产品路径。

## 下一步

1. **骨架对齐**：把采集笔画映射到标准骨架，残差才是真正的个人风格。
2. **序列模型**：RNN / Transformer 预测 `Δ` 点，显式建模抬落笔与速度。
3. **风格向量**：few-shot 多风格（8 字起步），而不是一人一模型。
4. **C++ 批量推理接入开发服务器**：让 dev server 走 `core`，彻底去掉 Python 推理。
