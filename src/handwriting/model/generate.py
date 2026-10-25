"""把训练好的风格模型作用到任意 ``StrokeSet``。

输入可以是汉字骨架、表格边框、思维导图连线，或导入的 SVG——
只要它是 ``StrokeSet``，模型就能给它加上「这个人的」偏移与笔压。
"""

from __future__ import annotations

import math
from collections.abc import Sequence
from itertools import pairwise

import numpy as np
import torch

from handwriting.model.data import (
    FEATURE_DIM,
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
    strength: float = 1.0,
) -> Stroke:
    """给一笔加上模型学到的风格。

    ``sample=False``（默认）取混合高斯的均值，输出平滑、可复现；
    ``sample=True`` 逐点抽样，能体现书写随机性，但相邻点可能跳到不同分量。
    ``strength`` 控制风格强度（0 = 原样，1 = 完整风格）。
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

        dx = dx_model / RESIDUAL_SCALE * strength
        dy = dy_model / RESIDUAL_SCALE * strength
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
    strength: float = 1.0,
) -> StrokeSet:
    """给一个 ``StrokeSet``（一个字 / 一个图形）整体加风格。"""
    return stylize_stroke_sets(model, config, [stroke_set], sample=sample, strength=strength)[0]


def stylize_stroke_sets(
    model: StyleResidualNet,
    config: TrainConfig,
    stroke_sets: Sequence[StrokeSet],
    sample: bool = False,
    strength: float = 1.0,
) -> list[StrokeSet]:
    """批量给多个 ``StrokeSet`` 加风格。

    把整篇文章的所有笔画摊平成一批，每个采样点只做一次前向——
    长文不会因为逐字调用而慢到不可用。
    """
    device = _model_device(model)
    model.eval()
    n = config.n_points

    flat: list[Stroke] = []
    owner: list[tuple[int, int]] = []
    for si, stroke_set in enumerate(stroke_sets):
        for ki, stroke in enumerate(stroke_set.strokes):
            flat.append(stroke)
            owner.append((si, ki))
    if not flat:
        return [StrokeSet([], ss.char) for ss in stroke_sets]

    centers = []
    lengths = []
    for stroke in flat:
        pts = resample_n(stroke.points, n)
        centers.append(smooth_points(pts, config.smooth_window))
        lengths.append(min(2.0, _length(pts)))

    count = len(flat)
    prev_res = torch.zeros((count, 2), device=device)
    prev_p = torch.full((count,), 0.5, device=device)
    collected: list[list[Point]] = [[] for _ in range(count)]
    feat_np = np.empty((count, FEATURE_DIM), dtype=np.float32)

    for i in range(n):
        for b in range(count):
            feat_np[b] = point_feature(
                centers[b],
                i,
                lengths[b],
                (float(prev_res[b, 0]), float(prev_res[b, 1])),
                float(prev_p[b]),
            )
        feats = torch.from_numpy(feat_np).to(device)
        with torch.no_grad():
            pi, mu, sigma, p_mu, p_sigma = model(feats)

        if sample:
            k = torch.multinomial(pi, 1)  # (B, 1)
            idx = k.unsqueeze(-1).expand(count, 1, 2)
            mu_k = mu.gather(1, idx).squeeze(1)
            sigma_k = sigma.gather(1, idx).squeeze(1)
            res = mu_k + sigma_k * torch.randn_like(mu_k)
            pressure = p_mu + p_sigma * torch.randn_like(p_mu)
        else:
            res = (pi.unsqueeze(-1) * mu).sum(dim=1)
            pressure = p_mu

        prev_res = res
        prev_p = pressure
        dx = (res[:, 0] / RESIDUAL_SCALE * strength).clamp(-_MAX_RESIDUAL, _MAX_RESIDUAL)
        dy = (res[:, 1] / RESIDUAL_SCALE * strength).clamp(-_MAX_RESIDUAL, _MAX_RESIDUAL)
        pressure = pressure.clamp(0.0, 1.0)

        for b in range(count):
            c = centers[b][i]
            collected[b].append(
                Point(
                    c.x + float(dx[b]),
                    c.y + float(dy[b]),
                    float(pressure[b]),
                    c.t,
                )
            )

    by_set: list[dict[int, Stroke]] = [{} for _ in stroke_sets]
    for b, (si, ki) in enumerate(owner):
        by_set[si][ki] = Stroke(collected[b])

    result: list[StrokeSet] = []
    for stroke_set, mapping in zip(stroke_sets, by_set, strict=True):
        strokes = [mapping[k] for k in range(len(stroke_set.strokes))]
        result.append(StrokeSet(strokes, stroke_set.char))
    return result
