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
| 横条本版面（字压横线） | 可用 |
| Qt 桌面预览工具 | 可用 |
| SVG 预览 | 可用 |
| 轨迹数据结构（C++ / Python） | 可用 |
| 数据集读取与质检 | 可用 |
| Qt 桌面 UI | **可用**（参数面板 + 实时预览 + 导出 + **串口直连书写**） |
| 串口 / GRBL 通信（C++ 核心） | 可用（流控、状态机、安全检查，118 项断言覆盖） |
| 工作区标定（软限位） | 可用（4 步向导 + 示意图 + 越界保护） |
| 笔迹风格模型 | 未开始 |
| 运动约束优化（论文核心） | 未开始 |

## 快速开始

### 1. Qt 预览工具（开发期主力）

```bash
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH="C:/Qt/6.9.3/mingw_64"
cmake --build build -j
./build/app/handwriting-studio.exe
```

左侧改参数、右侧实时出预览。**预览与导出的 G 代码共用同一份版面数据**，
所以看到的位置就是机器实际走的位置。

默认版面就是**横条本**（150×213 mm / 22 行，字压在横线上），
字宽默认等于行距、每行字数自动算 —— 正是作业本去掉页边距后的书写区。

### 1.1 串口直连书写（替代奎享雕刻）

底部「机器控制 · 串口直连」面板提供了与奎享雕刻类似的接口，软件内直接连 GRBL 控制板：

1. 插上控制板（Arduino Uno + CNC Shield / ESP32 等跑 GRBL 的板子），系统会枚举出 COM 口
2. 下拉选对应的串口，点 **连接**（默认 115200）
3. 连上后自动读 `$$` 拿行程与最大进给，做上机前安全检查
4. 先用 **回零 `$H`** 把笔归零，用 **点动** 把笔移到纸面左上角，再 **设零点 `G10 L20 P1`**
5. 左边调好文字与版面后，点 **开始书写** —— 程序会带行号流控、按 GRBL 的 128 B 串口缓冲限速发送

手动指令（回零 / 点动 / 设零点 / 抬落笔）与书写程序严格分离：**载入程序后没点「开始」之前，随手点回零不会把整个程序偷发下去**——这是个真实的安全隐患，已有回归测试守着。

### 1.2 工作区标定（软限位，**保护传动皮带**）

步进机没有位置反馈，**上电后手推机头不会更新 GRBL 坐标**——所以通电后必须重新标定「设备能走到哪里」，否则 G 代码发下去机头可能冲到机械端点扯断皮带。

标定流程（底部「工作区标定（软限位）」分组）：

1. **断电手推**机头到**最左上角**（物理起点，X 最小、Y 向上最大）
2. **通电**点「开始标定」，按向导走 4 步：
   - ① 记录**最左上角**（若通电前已手推到位，直接点「记录此角」）
   - ② 用点动把机头移到**最右上角**，点「记录此角」
   - ③ 用点动把机头移到**最右下角**，点「记录此角」
   - ④ 用点动把机头移到**最左下角**，点「记录此角」
3. 点「保存边界」—— 写入 `data/workspace-bounds.json`，重启软件后自动加载
4. 之后**普通点动会被限制在边界内**（越界自动贴边）；**发送 G 代码前**会自动对照矩形做安全检查
5. 示意图**实时显示机头位置与边界线**，机头越界立刻变红警告

记录顺序打乱也没关系——内部会自动取 min/max 归一化，所以无论你的坐标系方向如何都能得到正确矩形。

> **MinGW 构建注意**：必须静态链接 libstdc++（`app/CMakeLists.txt` 已配置）。
> Qt 的 mingw_64 包自带一个用较旧 MinGW 编译的 `libstdc++-6.dll`，
> 运行时会优先加载它，构造 `std::ifstream` 这类触及 `std::locale` 的对象
> 就会在该 DLL 内崩溃，而且栈回溯只指向 libstdc++，完全看不出是自己的代码问题。

### 1.3 打包与直接运行（无需在系统里装 Qt）

构建完成后，可执行文件位于：

```
build/app/handwriting-studio.exe
```

`app/CMakeLists.txt` 已在 **POST_BUILD** 阶段自动把运行所需的 DLL 与插件
复制到该目录，所以**整个 `build/app/` 文件夹拷到任意没有装 Qt 的 Windows 机器上，
双击 `handwriting-studio.exe` 就能跑**，不会再报“找不到 xxx.dll”。

自动跟随 exe 一起部署的文件（来自对 PE 导入表的完整闭包分析）：

| 类别 | 文件 |
| --- | --- |
| Qt 运行时 | `Qt6Core.dll` `Qt6Gui.dll` `Qt6Widgets.dll` |
| MinGW 运行时 | `libgcc_s_seh-1.dll` `libstdc++-6.dll` `libwinpthread-1.dll` |
| 平台插件 | `platforms/qwindows.dll`（普通 GUI 必选）、`platforms/qoffscreen.dll`（无头快照/演示必选） |
| 图像插件 | `imageformats/qjpeg.dll` `qico.dll` `qgif.dll` `qsvg.dll`（导出 PNG 等需要） |

> **分发要点**：务必把 `build/app/` **整个目录**一起拷走，不要只拷 `handwriting-studio.exe`——
> 上面这些 DLL 和 `platforms/`、`imageformats/` 子目录必须跟 exe 待在同一目录。

> **连机前驱动**：控制板（CH340 / CP2102 等 USB 转串口芯片）首次插上，Windows 可能没枚举出
> COM 口。请先安装对应厂家的 **VCP 虚拟串口驱动**，设备管理器里出现 `COMx` 后软件才能连上。

### 2. 命令行

```bash
pip install -e ".[dev]"

# 预取字形（需要联网，之后可离线使用）
python -m handwriting.cli preload "春眠不觉晓"

# 按横条本排版（默认）
python -m handwriting.cli render "春眠不觉晓" -o chun.nc --svg chun.svg

# 自由版面，导到空白纸
python -m handwriting.cli render "春眠不觉晓" --mode grid --char-size 20
```

### 3. 采集你的笔迹

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

**两种采集模式**（页面左上角切换）：

- **单字**：一个字一格，坐标归一化到字格 —— 用于**笔画骨架 / 单字形态**，
  按上面的覆盖度顺序写 150 字。
- **整行**：按横线**连续书写整行**（可连笔），坐标归一化到整行方框、`strokeStartT` 以整行起笔为基准 ——
  用于**位置飘逸 / 字距 / 行气 / 连笔潦草**，这些是单字采不到的。
  导出 `handwriting-capture/2.0`；加载时用 `LineSample.segment()`
  按「抬笔间隔 + 已知字数」自动分段，无需参考字形。

建议两条腿走：**整行写句子**（拿行级风格）+ **单字按字表补覆盖**（拿笔画结构）。
整行若记录了分段，还能切出单字样本（`split_chars()`），一份数据两用。

### 4. 构建 C++ 核心库

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

### 5. Python 侧（数据集与训练）

```bash
pip install -e ".[dev]"
pytest                      # 262 个用例，联网用例默认跳过
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
