# 0007. 运行时代码用 C++，Python 仅用于训练与开发工具

- 状态：已接受
- 日期：2026-10-26
- 相关：[ADR-0003](0003-cpp-core-python-for-training.md)（C++ 核心 + Python 训练）

## 背景

风格模型 v0 的推理最初写在 Python（`src/handwriting/model/generate.py`）。
如果产品运行时调用它，发布产物就必须带 Python 解释器：安装包从几十 MB 回到 GB 级，
启动多几百毫秒，部署也多一层依赖。这违背 ADR-0003 的分工。

用户明确要求：「比较底层的尽量用 C 语言，全局都这样。」

## 决策

1. **产品运行时（`core/`、`app/`、发布产物）只用 C++17**：
   数据结构、字形解析、版面、G 代码、SVG、串口、**模型推理**。
2. **Python 只用于**：
   - 训练（PyTorch）；
   - 开发期工具（采集器是 JS，`scripts/` 下的对比页 / 开发服务器）；
   - 跨语言对拍的参考实现。
3. 开发工具**不得把运行时逻辑带进发布路径**；需要推理 / 排版时优先调用 C++ 核心。
4. 新增运行时功能先写 C++ + 测试；Python 版本只能作为训练期参考。
5. 模型权重由 Python 导出为 `handwriting-style-model/1` JSON，
   C++ `style::Model` 加载；数值一致性由 `hr-style-dump` 对拍。

## 理由

- **部署**：C++ 无 Python 运行时，安装包小、启动快、依赖少。
- **单一事实来源**：运行时只有一份 C++ 实现，避免 Python/C++ 两份实现悄悄漂移。
- **同语言**：与 Qt 前端、`core`、G 代码同语言，无 FFI / 子进程胶水。
- **为什么不是纯 C**：`core` 与 Qt 已经是 C++17；引入纯 C 要重写类、容器与 Qt 交互，
  收益只是「更 C」，代价是多一层胶水。**C++ 就是这里的底层语言。**

## 后果

正面：

- 运行时零 Python 依赖；推理与排版共用一份核心；性能可预测。

代价与迁移：

- C++ 迭代比 Python 慢；训练实验仍在 Python，需要「导出 + 对拍」。
- `src/handwriting/model/generate.py` 降级为**训练期参考实现**；
  产品路径改用 `core/src/style_model.cpp`。
- `scripts/serve_dev.py` 目前仍调用 Python 参考实现（属于开发工具）；
  下一步改为调用 C++ 批量推理工具。
- 当前对拍结果：C++ 与 Python 在同一输入上最大绝对误差 **1.14e-07**（float32 精度）。

## 备选方案

| 方案 | 否决理由 |
|---|---|
| 继续 Python 推理 | 安装包 / 启动 / 部署退化，违背 ADR-0003 |
| 纯 C | 与 Qt/C++ 核心冲突，需重写容器与类，收益低 |
| Python 推理 + pybind11 | 仍需 Python 运行时，没解决部署问题 |

## 更新

- 2026-10-26：`scripts/serve_dev.py` 已改为默认调用 C++ `hr-style-batch`，
  Python 仅作回退；开发者模式支持预览内直接编辑。
