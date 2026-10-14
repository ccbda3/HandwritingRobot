# HandwritingRobot

学习个人笔迹风格，生成带真实书写时序的笔画轨迹，导出 GRBL G 代码驱动写字机器人。

**与现有写字软件的本质区别**：传统工具只是描图片轮廓，没有笔顺、没有时序，写出来僵硬如机器描图。
本项目处理的是**联机笔迹**（笔尖坐标 + 笔序 + 时序 + 抬落笔 + 压感），复刻的是人的书写动作。

```
手写样本 ──▶ 风格模型 ──▶ 运动约束优化 ──▶ G 代码 ──▶ 写字机器人
              (Python)        (C++ core)      (C++ core)
```

## 当前状态

**Pre-Alpha。** 采集链路、字形 → G 代码链路已跑通，风格模型尚未开始。

| 模块 | 状态 |
|---|---|
| 笔迹采集器（网页） | 可用 |
| 风格特征提取 | 可用 |
| 主动选字（覆盖度驱动） | 可用 |
| 字形骨架 → 轨迹 → G 代码 | 可用 |
| SVG 预览 | 可用 |
| 轨迹数据结构（C++ / Python） | 可用 |
| 数据集读取与质检 | 可用 |
| G 代码生成 | 可用（基础版） |
| Qt 桌面 UI | 骨架 |
| 笔迹风格模型 | 未开始 |
| 运动约束优化（论文核心） | 未开始 |

## 快速开始

### 1. 先看字能不能写出来（不需要模型，不需要硬件）

```bash
pip install -e ".[dev]"

# 预取字形数据（需要联网，之后可离线使用）
python -m handwriting.cli preload "永字八法"

# 生成 G 代码 + SVG 预览
python -m handwriting.cli render "永字八法" -o yong.nc --svg yong.svg

# 只看统计
python -m handwriting.cli render "永字八法" --dry-run
```

字形来自标准笔顺库（[Make Me a Hanzi](https://github.com/skishore/makemeahanzi)
派生的 hanzi-writer-data），**medians 字段自带正确笔顺**。
用浏览器打开导出的 SVG 就能看到机器会画成什么样，不必先接上机器。

管线细节、坐标系标定与已知限制见 [docs/glyph-pipeline.md](docs/glyph-pipeline.md)。

### 2. 采集你的笔迹

用浏览器打开 [`capture/index.html`](capture/index.html)，按提示写 150 个汉字，导出 JSON。
零依赖、可离线，手机 / 平板 / 数位板 / 电脑都能用。

**采集顺序不是字表原序，而是按「结构 × 复杂度」覆盖度排的。**
前 8 个字是人工策展的冷启动集（`永西叶好冬花国风`），覆盖 8 个不同的结构-复杂度格子；
写完 40 个字，每一类都至少有 2 个样本。这样在同样的字数预算下，
风格覆盖比按使用频率顺序写好得多。

页面顶部实时显示**压感是否支持**与**实际采样率**，侧栏有**质量检查**面板——
采样率不足、压感缺失、频繁出格会在写的当下就提示，不必等采完 150 个字才发现整批报废。

推荐的输入设备（不需要自己造硬件）：

| 方案 | 成本 | 采样率 | 压感 |
|---|---|---|---|
| 入门数位板（绘王 / 高漫 6×4 英寸） | 100–200 元 | 133–200 Hz | 有 |
| iPad + Apple Pencil | 已有则 0 | 120–240 Hz | 有 |
| 手机触屏 + 电容笔 | 20–50 元 | 60–120 Hz | 无 |
| 鼠标 | 0 | 60–125 Hz | 无（仅能验证链路） |

### 3. 构建 C++ 核心库

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

只构建核心库与测试（不需要 Qt）：

```bash
cmake -S . -B build -DHR_BUILD_APP=OFF
```

Qt 前端优先使用 Qt6；找不到时自动回退 Qt5.15，便于本地开发机快速构建。

### 4. Python 侧（数据集与训练）

```bash
pip install -e ".[dev]"
pytest                      # 227 个用例，联网用例默认跳过
pytest --run-network -m network   # 验证真实字形数据
ruff check src tests
```

## 目录结构

```
HandwritingRobot/
├── core/                 C++17 核心库（无 Qt、无 Python 依赖）
│   ├── include/handwriting/
│   ├── src/
│   └── tests/
├── app/                  Qt 桌面前端（直接链接 core）
├── src/handwriting/      Python：字形、渲染、风格、数据集、G 代码、CLI
├── capture/index.html    笔迹采集器
├── benchmarks/           性能基准（C++ 与 Python 同负载对比）
├── scripts/              开发期工具（生成字表顺序等）
├── docs/
│   ├── architecture.md    系统架构与语言分工
│   ├── glyph-pipeline.md  字形 → 轨迹 → G 代码管线
│   ├── data-format.md     笔迹数据格式规范
│   └── adr/               架构决策记录
├── data/                 本地采集数据与导出（**不入库**）
└── tests/                Python 测试
```

## 为什么是 C++ 核心 + Python 训练

用相同合成负载（100 字 × 8 笔 × 60 点）实测：

| 环节 | Python | C++ |
|---|---|---|
| 轨迹归一化 | 65.15 ms | 0.13 ms |
| G 代码生成 | 65.67 ms | 37.42 ms |
| 串口传输 | 130.5 s | 130.5 s |
| 机床实际运行 | ~600 s | ~600 s |
| **计算占端到端** | **0.018%** | **0.005%** |

**换语言把端到端从 730.8 秒降到 730.6 秒。** 批处理吞吐不是选 C++ 的理由——
瓶颈是串口带宽与机械运动。真正的理由是**交互延迟**：拖动参数滑块要实时重算预览，
Python 的 65 ms 是 15 fps，C++ 的 0.13 ms 无感。加上发布版可彻底摆脱 Python 运行时
（安装包 2~3 GB → 几十 MB）。

模型训练仍留在 Python：PyTorch 的自动微分与 CUDA kernel 库只服务 Python，
用 C 重写训练代码等于放弃整个生态。

详见 [docs/architecture.md](docs/architecture.md) 与 [ADR-0003](docs/adr/0003-cpp-core-python-for-training.md)。

## 文档

- [系统架构](docs/architecture.md) —— 数据流、语言分工、GRBL 硬约束
- [数据格式规范](docs/data-format.md) —— `handwriting-capture/1.0`
- [架构决策记录](docs/adr/) —— 每个不可逆决策的理由
- [贡献指南](CONTRIBUTING.md)

## 许可

代码采用 [Apache-2.0](LICENSE)。

**第三方数据与模型权重单独声明许可**，不适用仓库根目录的 LICENSE。
详见 [ADR-0004](docs/adr/0004-license-and-third-party-data.md)。

`data/` 下的用户笔迹数据**不会**进入版本库——它是生物特征数据，隐私敏感。

## 环境要求

- C++：CMake ≥ 3.21，支持 C++17 的编译器（MSVC 2019+ / GCC 11+ / Clang 14+）
- Qt：Qt 6.4+（推荐）或 Qt 5.15
- Python：≥ 3.10
- 本地推理显卡：NVIDIA RTX 5060 8GB —— 注意这是 Blackwell 架构（sm_120），
  **必须使用 PyTorch ≥ 2.7 + CUDA 12.8**，旧版 wheel 没有对应 kernel。
  **不要使用 TensorFlow 1.x 时代的代码**，它在这张卡上无法运行。
