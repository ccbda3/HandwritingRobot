# 笔迹数据格式规范

本文件定义两种采集格式：

- `handwriting-capture/1.0` —— **单字**采集：一个字一格、归一化到字格，
  用于笔画骨架 / 单字形态（下面先讲这种）。
- `handwriting-capture/2.0` —— **整行**采集：归一化到整行方框、保留字与字之间的
  相对位置，用于位置飘逸 / 字距 / 行气 / 连笔潦草 / 字间节奏（见文末「整行采集格式」）。

## 顶层结构

```json
{
  "format": "handwriting-capture/1.0",
  "meta": { "...": "..." },
  "samples": [ "...": "..." ]
}
```

## meta

| 字段 | 类型 | 说明 |
|---|---|---|
| `device` | string | 采集设备描述，如 `触控笔 / 数位笔` |
| `pressureSupported` | bool | 是否采到真实压感。**为 false 时无法还原笔画粗细变化** |
| `samplingHz` | int \| null | 实测采样率。**低于 100 Hz 时快速连笔会丢细节** |
| `charCount` | int | 字表长度 |
| `cellPx` | int | 采集方格边长（像素），用于还原绝对尺度 |
| `devicePixelRatio` | number | 设备像素比 |
| `coordSystem` | string | 坐标系说明 |
| `orderStrategy` | string | 字表排序策略说明 |
| `createdAt` | string | ISO 8601 时间戳 |

## samples[]

| 字段 | 类型 | 说明 |
|---|---|---|
| `index` | int | 样本序号 |
| `char` | string | 字符标签 |
| `strokeCount` | int | 笔画数 |
| `rate` | int | 该字的实测采样率 |
| `pressure` | bool | 该字是否带压感 |
| `strokes` | array | 笔画数组，**外层顺序即笔顺** |
| `strokeStartT` | array[int] | 可选。每笔相对**该字起笔时刻**的毫秒数 |

### 为什么需要 strokeStartT

`strokes` 里每个点的 `t` 是相对**该笔**起笔的，因此无法还原笔与笔之间的间隔。
而「抬笔时间 / 总书写时间」是判断书写连贯性的关键风格特征
（见 `handwriting.style.StyleFeatures.pause_ratio`）。

`strokeStartT` 补上这个信息：第 i 笔的绝对起始时刻 = `strokeStartT[i]`。
缺少该字段时 `pause_ratio` 为 `None`，其余特征不受影响。

这是**向后兼容的新增字段**，旧版采集器导出的数据仍然合法。

## strokes 内的采样点

每个采样点是四元组 `[x, y, pressure, t]`：

| 位置 | 类型 | 范围 | 说明 |
|---|---|---|---|
| 0 | float | 0~1（可略微越界） | x 坐标，向右为正 |
| 1 | float | 0~1（可略微越界） | y 坐标，**向下为正** |
| 2 | float | 0~1 | 压感。设备不支持时恒为 0.5 |
| 3 | int | ≥ 0 | 相对该笔起笔的毫秒数，同一笔内单调不减 |

坐标原点在方格左上角，归一化到方格边长。
`x`/`y` 允许略微超出 0~1（手写会出格），采集器会同时记录出格点数用于质量筛选。

## 完整示例

```json
{
  "format": "handwriting-capture/1.0",
  "meta": {
    "device": "触控笔 / 数位笔",
    "pressureSupported": true,
    "samplingHz": 150,
    "charCount": 150,
    "cellPx": 512,
    "coordSystem": "归一化到方格，原点在方格左上角，x 向右，y 向下，取值 0~1",
    "orderStrategy": "structure×complexity 覆盖度轮转贪心（scripts/build_curriculum.py）",
    "createdAt": "2026-10-14T15:59:00.000Z"
  },
  "samples": [
    {
      "index": 0,
      "char": "永",
      "strokeCount": 5,
      "rate": 150,
      "pressure": true,
      "strokes": [
        [[0.512, 0.104, 0.31, 0], [0.508, 0.118, 0.44, 7]],
        [[0.300, 0.300, 0.52, 0]]
      ],
      "strokeStartT": [0, 420]
    }
  ]
}
```

## 为什么笔顺不需要"恢复"

传统做法是把汉字当图像处理，因此必须从像素反推笔顺——这是一个没有唯一解的逆问题。
本格式把笔顺编码为 `strokes` 的**外层数组顺序**：第 0 笔、第 1 笔、第 2 笔……
信息在采集时就完整记录了，不需要任何推断。

## 采集顺序

采集器**不按字表原序**要求用户书写，而是按 `scripts/build_curriculum.py` 算出的
「结构 × 复杂度」覆盖度顺序：

- 前 8 个字是人工策展的冷启动集（`永西叶好冬花国风`），
  覆盖 8 个不同的结构-复杂度格子。其中「永」居首是因为永字八法包含
  横竖撇捺点折钩提全部八种基本笔画
- 之后按轮转贪心补齐：每次从当前覆盖次数最少的格子取一个字
- 写完 40 字后，每个格子都至少有 2 个样本

顺序与元数据以字符串常量内嵌在 `capture/index.html` 中。
`tests/test_curriculum.py::TestCollectorConsistency` 会检查两者是否同步。

## 采集质量门槛

`CaptureData.validate()` 会检查以下项目：

- `pressureSupported` 必须为 true，否则无法还原笔画粗细
- `samplingHz` 建议 ≥ 100 Hz
- 每个样本必须有字符标签且至少一笔

采集器在书写过程中也会实时显示这些检查结果（质量检查面板）。

## 整行采集格式（`handwriting-capture/2.0`）

单字采集把每个字归一化进同一个方格，会**抹掉字与字之间的相对位置**，
因此采不到位置飘逸、字距、行气、连笔潦草、字间节奏。整行格式补上这些。

### 与 1.0 的区别

| 维度 | 1.0（单字） | 2.0（整行） |
|---|---|---|
| 顶层字段 | `samples` | `lines` |
| 归一化基准 | 单个字格 | 一整行方框 |
| `strokeStartT` 基准 | 该字起笔 | **整行起笔** |
| 笔顺表达 | `strokes` 外层顺序 | 同左 |
| 字边界 | 一个 sample 即一个字 | `charRanges` 显式给出每个字占用的笔画区间 |

### 顶层结构

```json
{
  "format": "handwriting-capture/2.0",
  "meta": { "...": "..." },
  "lines": [ "...": "..." ]
}
```

`meta` 字段与 1.0 相同，另建议增加 `lineCount`。

### lines[]

| 字段 | 类型 | 说明 |
|---|---|---|
| `index` | int | 行序号 |
| `text` | string | **这一行写的文本**，如 `"春眠不觉晓"`。字数即目标分段数 |
| `strokeCount` | int | 笔画数 |
| `rate` | int | 实测采样率 |
| `pressure` | bool | 是否带压感 |
| `strokes` | array | 与 1.0 同结构；坐标归一化到**整行方框** |
| `strokeStartT` | array[int] | 每笔相对**整行起笔**的毫秒数，与 `strokes` 等长 |
| `charRanges` | array[[int,int]] | 可选。每个字占用的笔画区间 `[start, end)`，与 `text` 逐字对应 |

### charRanges 是「标注」不是「输入」

`charRanges` 可以采集时由人确认，也可以事后由 `handwriting.segment.segment_line`
用「抬笔间隔 + 字数」自动算出来（`LineSample.segment()`）。它必须满足：

- 数量等于 `len(text)`；
- 首尾相接、覆盖 `[0, strokeCount)`，不重不漏。

有了它就能把整行切成单字样本（`LineSample.split_chars()`），一份数据两用。

### 分段为什么可行

在线书写带时序：同一字内笔画间隔短或为 0，字与字之间通常有明显停顿。
再叠加「已知文本的字数」这一约束，分段就变成受约束的对齐，而不是盲猜。
实现见 `src/handwriting/segment.py`。

## 版本演进规则

- 字段只增不改不删；废弃字段标记 `deprecated` 并保留至少一个大版本
- `format` 字符串随不兼容变更递增主版本号
- 读取端遇到未知 `format` 必须报错，不得静默降级
