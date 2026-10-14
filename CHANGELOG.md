# 更新日志

本文件记录所有值得注意的变更。

格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

## [未发布]

### 新增

- **风格特征提取** `handwriting.style`：从轨迹中提取 14 维风格向量
  （倾角、宽高比、墨迹密度、直线度、转折角、折笔锐度与密度、笔画长度变异、
  压感均值/标准差/提按、书写速度均值/标准差、抬笔比），
  并带可用性掩码——无压感设备的压感特征为 `None` 而非 0
- **主动选字** `handwriting.curriculum`：150 字表带「结构 × 复杂度」元数据，
  按覆盖度轮转贪心排序。前 8 个字人工策展为覆盖 8 个不同格子，
  以「永」居首（永字八法含全部八种基本笔画）
- **采集器升级**：按覆盖度顺序提示待写字、实时质量检查面板、
  按结构着色的进度网格、8/40/150 字里程碑、导出新增 `strokeStartT` 字段
- `scripts/build_curriculum.py`：生成字表顺序与元数据常量
- C++17 核心库 `core/`：轨迹数据结构、几何变换、等比归一化、G 代码生成
- Python 侧 `src/handwriting/`：轨迹数据结构、数据集读取与质量体检、G 代码生成
- 性能基准：`benchmarks/cpp/` 与 `benchmarks/python/` 使用相同合成负载，可直接对比
- 单元测试：C++ 2 个测试程序（CTest）、Python 119 个用例
- 文档：系统架构、数据格式规范、5 条架构决策记录
- 工程规范：Apache-2.0 许可、`.editorconfig`、`.gitattributes`、`.clang-format`、
  pre-commit 钩子、GitHub Actions CI、Issue/PR 模板

### 变更

- **架构调整**：原「Qt 前端 + Python 子进程 + JSON 通信」方案被
  [ADR-0003](docs/adr/0003-cpp-core-python-for-training.md) 取代。
  改为 C++ 核心库 + Python 仅用于训练，Qt 前端直接链接核心库。
- **输入通道调整**：原「以手写照片为主要输入」被
  [ADR-0002](docs/adr/0002-online-trajectory-over-offline-images.md) 取代。
  改为以联机笔迹为主通道，照片降级为可选的第二通道。
- **采集与学习策略**：[ADR-0005](docs/adr/0005-progressive-guided-collection.md)
  明确「实时采集 + 前向推理 + 离线微调」三阶段，不做逐字增量训练。
- `perf(core)`：G 代码生成的数字格式化从 `ostringstream` 换成 `snprintf`，
  48000 点负载下从 63.12 ms 降到 37.42 ms

### 修复

- 单点笔画（点）此前不产生任何 G 代码，现在会落笔、驻留 0.05 s、抬笔，
  在纸面留下墨点
- 重采样末段短于 1e-6 时不再补端点。此前浮点累积（如 25 × 0.02 ≠ 0.5）
  会产生长度接近 0 的末段，其方向由浮点噪声主导，算出的转折角完全错误

### 安全

- `.gitignore` 强制忽略 `data/**` 下的用户笔迹数据与模型权重，
  避免生物特征数据进入公开仓库

[未发布]: https://github.com/Whaoy/HandwritingRobot/commits/main
