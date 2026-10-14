# data/ —— 本地数据目录

**本目录下的内容不会进入版本库。**

## 为什么

这里存放的是**用户本人的笔迹数据**。笔迹属于生物特征数据，隐私敏感度高于普通个人信息，
不应该出现在公开仓库里。`.gitignore` 已忽略本目录下所有 `json` / `csv` / `npz` / `npy` / 图片文件，
只保留本文件与 `.gitkeep`。

同样被忽略的还有：模型权重（`*.pt` / `*.pth` / `*.onnx` / `*.safetensors`）、
训练产物（`checkpoints/` / `runs/` / `outputs/`）。

## 建议的目录结构

```
data/
├── raw/             采集器直接导出的原始 JSON（按日期/批次分目录）
├── processed/       清洗、归一化后的训练集
└── exports/         生成的 G 代码
```

`raw/`、`processed/` 也已被 `.gitignore` 忽略。

## 采集流程

1. 浏览器打开 `../capture/index.html`
2. 写 150 个汉字（页面内置字表），每 20~30 字导出一次 JSON 备份
3. 把导出的 JSON 放进 `raw/`
4. 用 `handwriting.dataset.CaptureData.load()` 读取并跑 `validate()` 做质量体检

```python
from handwriting import CaptureData

data = CaptureData.load("data/raw/handwriting-20261014-2359.json")
print(data.char_count, data.total_points, data.average_points_per_char)

for problem in data.validate():
    print("问题:", problem)
```

## 质量门槛

`validate()` 会检查：

- **压感**：`pressureSupported` 为 false 时无法还原笔画粗细变化，需要换设备重采
- **采样率**：低于 100 Hz 时快速连笔会丢细节
- **完整性**：每个样本必须有字符标签且至少一笔

## 许可提醒

**不要把受限数据集或其派生文件放进本目录然后提交。**
CASIA-OLHWDB、SCUT-COUCH 等仅限非商业研究使用；
WriteLikeYou 的训练数据明确标注 commercial use prohibited。

详见 [ADR-0004](../docs/adr/0004-license-and-third-party-data.md)。
