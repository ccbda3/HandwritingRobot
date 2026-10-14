# 更新日志

本文件记录所有值得注意的变更。

格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

## [未发布]

### 新增

- 笔迹采集器 `capture/index.html`：单文件网页，零依赖可离线运行；
  内置 150 字表；采集带笔序、时序、抬落笔与压感的联机笔迹；
  自动检测压感支持与实际采样率；导出 JSON / CSV
- C++17 核心库 `core/`：轨迹数据结构、几何变换、等比归一化、G 代码生成
- Python 侧 `src/handwriting/`：轨迹数据结构、数据集读取与质量体检、G 代码生成
- 性能基准：`benchmarks/cpp/bench_pipeline.cpp` 与 `benchmarks/python/bench_pipeline.py`
  使用相同合成负载，可直接对比
- 单元测试：C++ 2 个测试程序（CTest）、Python 46 个用例
- 文档：系统架构、数据格式规范、4 条架构决策记录
- 工程规范：Apache-2.0 许可、`.editorconfig`、`.gitattributes`、`.clang-format`、
  pre-commit 钩子、GitHub Actions CI、Issue/PR 模板

### 变更

- **架构调整**：原「Qt 前端 + Python 子进程 + JSON 通信」方案被
  [ADR-0003](docs/adr/0003-cpp-core-python-for-training.md) 取代。
  改为 C++ 核心库 + Python 仅用于训练，Qt 前端直接链接核心库。
- **输入通道调整**：原「以手写照片为主要输入」被
  [ADR-0002](docs/adr/0002-online-trajectory-over-offline-images.md) 取代。
  改为以联机笔迹为主通道，照片降级为可选的第二通道。
- `perf(core)`：G 代码生成的数字格式化从 `ostringstream` 换成 `snprintf`，
  48000 点负载下从 63.12 ms 降到 37.42 ms

### 修复

- 单点笔画（点）此前不产生任何 G 代码，现在会落笔、驻留 0.05 s、抬笔，
  在纸面留下墨点

### 安全

- `.gitignore` 强制忽略 `data/**` 下的用户笔迹数据与模型权重，
  避免生物特征数据进入公开仓库

[未发布]: https://github.com/Whaoy/HandwritingRobot/commits/main
