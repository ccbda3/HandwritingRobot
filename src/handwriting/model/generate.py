"""把训练好的风格模型作用到任意 ``StrokeSet``。

输入可以是汉字骨架、表格边框、思维导图连线，或导入的 SVG——
只要它是 ``StrokeSet``，模型就能给它加上「这个人的」偏移与笔压。
"""

from __future__ import annotations

import math
from itertools import pairwise

import torch

from handwriting.model.data import (
    RESIDUAL_SCALE,
    point_feature,
    resample_n,
    smooth_points,
)
from handwriting.model.mdn import sample_component
from handwriting.model.net import StyleResidualNet
from handwriting.model.train import TrainConfig
from handwriting.stroke import Point, Stroke, StrokeSet

_MAX_RESIDUAL = 0.2


def _model_device(model: StyleResidualNet) -> torch.device:
    try:
        return next(model.parameters()).device
    except StopIteration:  # pragma: no cover
        return torch.device("cpu")


def _length(points: list[Point]) -> float:
    return sum(math.hypot(b.x - a.x, b.y - a.y) for a, b in pairwise(points))


def stylize_stroke(
    model: StyleResidualNet,
    config: TrainConfig,
    stroke: Stroke,
    sample: bool = False,
) -> Stroke:
    """给一笔加上模型学到的风格。

    ``sample=False``（默认）取混合高斯的均值，输出平滑、可复现；
    ``sample=True`` 逐点抽样，能体现书写随机性，但相邻点可能跳到不同分量。
    """
    device = _model_device(model)
    n = config.n_points
    pts = resample_n(stroke.points, n)
    center = smooth_points(pts, config.smooth_window)
    length_norm = min(2.0, _length(pts))

    out: list[Point] = []
    prev_res = (0.0, 0.0)
    prev_p = 0.5
    for i in range(n):
        feat = point_feature(center, i, length_norm, prev_res, prev_p)
        x = torch.tensor(feat, dtype=torch.float32, device=device).reshape(1, 1, -1)
        with torch.no_grad():
            pi, mu, sigma, p_mu, p_sigma = model(x)

        if sample:
            k = sample_component(pi[0, 0])
            dx_model = float(mu[0, 0, k, 0] + sigma[0, 0, k, 0] * torch.randn((), device=device))
            dy_model = float(mu[0, 0, k, 1] + sigma[0, 0, k, 1] * torch.randn((), device=device))
            pressure = float(p_mu[0, 0] + p_sigma[0, 0] * torch.randn((), device=device))
        else:
            dx_model = float((pi[0, 0] * mu[0, 0, :, 0]).sum())
            dy_model = float((pi[0, 0] * mu[0, 0, :, 1]).sum())
            pressure = float(p_mu[0, 0])

        # 特征里的「上一残差」用模型空间（放大后）的值，保持与训练一致
        prev_res = (dx_model, dy_model)

        dx = dx_model / RESIDUAL_SCALE
        dy = dy_model / RESIDUAL_SCALE
        dx = max(-_MAX_RESIDUAL, min(_MAX_RESIDUAL, dx))
        dy = max(-_MAX_RESIDUAL, min(_MAX_RESIDUAL, dy))
        pressure = min(1.0, max(0.0, pressure))

        out.append(Point(center[i].x + dx, center[i].y + dy, pressure, center[i].t))
        prev_p = pressure

    return Stroke(out)


def stylize_stroke_set(
    model: StyleResidualNet,
    config: TrainConfig,
    stroke_set: StrokeSet,
    sample: bool = False,
) -> StrokeSet:
    """给一个 ``StrokeSet``（一个字 / 一个图形）整体加风格。"""
    model.eval()
    strokes = [stylize_stroke(model, config, s, sample) for s in stroke_set.strokes]
    return StrokeSet(strokes, stroke_set.char)
