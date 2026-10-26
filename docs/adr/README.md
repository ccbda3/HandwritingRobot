# 架构决策记录（ADR）

本目录记录**不可轻易逆转**的技术决策及其理由。每条 ADR 一经接受就不再修改正文；
若需推翻，新写一条 ADR 并将旧条目标记为「已被 ADR-XXXX 取代」。

写 ADR 的门槛：这个决策如果选错了，返工成本超过一周。日常实现细节不写 ADR。

## 索引

| 编号 | 标题 | 状态 |
|---|---|---|
| [0001](0001-record-architecture-decisions.md) | 采用架构决策记录 | 已接受 |
| [0002](0002-online-trajectory-over-offline-images.md) | 以联机笔迹而非离线图像作为风格来源 | 已接受 |
| [0003](0003-cpp-core-python-for-training.md) | C++ 核心库 + Python 仅用于训练 | 已接受 |
| [0004](0004-license-and-third-party-data.md) | 许可策略与第三方数据合规 | 已接受 |
| [0005](0005-progressive-guided-collection.md) | 渐进式引导采集，而非实时增量训练 | 已接受 |
| [0006](0006-arbitrary-shapes-and-versatile-training.md) | 任意图形内容层与「全能」训练口径 | 已接受 |
| [0007](0007-runtime-in-cpp-python-for-training.md) | 运行时代码用 C++，Python 仅用于训练与开发工具 | 已接受 |

## 模板

```markdown
# NNNN. 决策标题

- 状态：提议 / 已接受 / 已被 ADR-XXXX 取代
- 日期：YYYY-MM-DD

## 背景

要解决什么问题？有哪些约束？

## 决策

我们决定做什么。

## 理由

为什么这么选。要有数据或事实支撑，不能只写「因为更好」。

## 后果

正面：得到什么。
负面：付出什么代价，接受什么风险。
```

## 待决策清单

- 推理后端：ONNX Runtime C++ vs LibTorch C++
- 笔迹风格模型选型
- 是否引入 pybind11 消除 Python/C++ 双实现
- 是否上 Git LFS 管理模型权重
