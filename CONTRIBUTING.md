# 贡献指南

## 开发环境

```bash
git clone https://github.com/Whaoy/HandwritingRobot.git
cd HandwritingRobot

# C++ 核心库
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DHR_BUILD_APP=OFF
cmake --build build -j
ctest --test-dir build --output-on-failure

# Python 侧
python -m venv .venv
.venv/Scripts/activate        # Windows
pip install -e ".[dev]"
pytest
ruff check src tests
ruff format --check src tests
```

提交前安装钩子，让检查自动跑：

```bash
pre-commit install
```

## 提交规范

使用 [Conventional Commits](https://www.conventionalcommits.org/)：

```
<type>(<scope>): <subject>

<body>

<footer>
```

**type**

| type | 用途 |
|---|---|
| `feat` | 新功能 |
| `fix` | 修复缺陷 |
| `perf` | 性能优化（必须附基准数据） |
| `refactor` | 重构，不改变外部行为 |
| `docs` | 文档 |
| `test` | 测试 |
| `build` | 构建系统、依赖 |
| `ci` | CI 配置 |
| `chore` | 杂项 |

**scope**：`core` / `app` / `capture` / `py` / `docs` / `bench` / `ci`

示例：

```
feat(core): 支持 G2/G3 圆弧压缩连续直线段

115200 baud 下 0.1mm 分段会让规划器饥饿，把曲率平缓的连续段
拟合成圆弧可将指令数降低约 40%。

Refs: #12
```

```
perf(core): 用 snprintf 替换 ostringstream 生成坐标

G 代码生成 63.12 ms -> 37.42 ms（48000 点负载）。

基准: benchmarks/cpp/bench_pipeline.cpp
```

## 分支与合并

- 单人开发阶段直接在 `main` 上提交
- 加入协作者后：在 GitHub 上把 `main` 设为受保护分支，并取消
  `.pre-commit-config.yaml` 中 `no-commit-to-branch` 的注释
- 分支命名：`feat/xxx`、`fix/xxx`、`perf/xxx`
- 合并方式：squash merge，保持 `main` 线性历史
- PR 必须通过 CI（Python lint + 测试、C++ 构建 + CTest）才能合并

## 代码规范

### 通用

- 编码 UTF-8，换行 LF（`.gitattributes` 已强制；`.bat`/`.ps1` 例外为 CRLF）
- 缩进 4 空格，行宽 100（见 `.editorconfig`、`.clang-format`、`ruff` 配置）
- 注释与文档用中文，**标识符用英文**
- 注释写「为什么」，不写「做了什么」——代码本身已经说明了后者

### C++

- C++17，禁止编译器扩展（`CMAKE_CXX_EXTENSIONS OFF`）
- `core/` **不得依赖 Qt**，保证可独立构建与测试
- 编译零警告：CI 开启 `-Wall -Wextra -Wpedantic -Werror`（MSVC 为 `/W4`）
- 头文件用 `#pragma once`
- 用 `[[nodiscard]]`、`constexpr`、`std::optional` 表达意图
- 避免裸 `new`/`delete`；用 RAII 与标准容器
- 格式化：`clang-format -i <file>`

### Python

- 最低版本 3.10；使用 `from __future__ import annotations`
- 所有函数与模块必须有类型标注
- 公共 API 必须有 docstring
- 工具链：`ruff`（lint + format）、`pytest`

## 性能改动的规矩

**任何声称「更快」的改动都必须附基准数据，否则不予合并。**

```bash
# C++ 侧
cmake --build build -j && ./build/benchmarks/cpp/bench_pipeline.exe

# Python 侧
python benchmarks/python/bench_pipeline.py
```

两个基准使用**完全相同的合成负载**，可直接对比。PR 描述里要给出改前/改后的数字。

同时请记住：本项目的端到端瓶颈是**串口带宽与机械运动**，计算只占 0.018%。
在优化计算之前，先确认它不是过早优化。

## 测试要求

- 新增功能必须带测试
- 修复缺陷必须先写能复现的测试
- C++ 测试放在 `core/tests/`，用 `core/tests/check.hpp`（不引入外部依赖）
- Python 测试放在 `tests/`
- **行为对齐**：`core/`（C++）与 `src/handwriting/`（Python）目前各有等价实现，
  任何一侧行为变更必须同步另一侧与对应测试，否则双实现会漂移

## 数据与隐私

- **禁止提交用户笔迹数据。** `.gitignore` 已忽略 `data/**` 下的 json/csv/npz/npy/图片
- 禁止提交受限数据集（CASIA-OLHWDB、SCUT-COUCH 等）或其派生文件
- 禁止提交禁止商用的模型权重
- 详见 [ADR-0004](docs/adr/0004-license-and-third-party-data.md)

## 架构决策

若你的改动涉及以下任一情况，**先写 ADR 再写代码**：

- 引入新的第三方依赖
- 改变 `core/` 与 `app/`、Python 之间的边界
- 改变数据格式或通信协议
- 任何返工成本超过一周的选择

模板见 [docs/adr/README.md](docs/adr/README.md)。
