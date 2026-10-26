"""第 3 层：笔尖序列模型（最小骨架）。

现有 v0 是「逐点残差」模型，缺少**节奏**：每笔独立、匀速、没有抬落笔。
这里换成序列模型：GRU 读入「下一段中心线 + 上一笔尖状态」，输出

- 下一个点的 ``Δx, Δy``（混合高斯，保留多种写法）
- 抬落笔概率（Bernoulli）
- 压感（高斯）

先用合成数据自测，等真实行数据到位后换数据即可。这是**训练期骨架**，
推理最终要按 ADR-0007 落到 C++。
"""

from __future__ import annotations

import math
from collections.abc import Callable, Sequence
from dataclasses import dataclass

import numpy as np
import torch
from torch import nn
from torch.nn import functional as F  # noqa: N812

from handwriting.model.data import resample_n
from handwriting.model.mdn import gaussian_nll, mdn_nll
from handwriting.stroke import Point, Stroke

FEATURE_DIM = 7
DELTA_SCALE = 50.0
"""Δ 点放大倍数：真实 Δ 只有 1e-2 量级，直接回归会让 σ 塌缩。"""

_EPS = 1e-4


@dataclass(slots=True)
class SequenceBatch:
    features: np.ndarray  # (B, N-1, FEATURE_DIM)
    delta: np.ndarray  # (B, N-1, 2)，已乘 DELTA_SCALE
    pressure: np.ndarray  # (B, N-1)
    pen_up: np.ndarray  # (B, N-1)


@dataclass(slots=True)
class SequenceConfig:
    n_points: int = 48
    hidden: int = 128
    mixtures: int = 5
    epochs: int = 300
    lr: float = 1e-3
    batch_size: int = 32
    seed: int = 0
    device: str = "cpu"
    pressure_weight: float = 0.5
    pen_weight: float = 0.2


def build_sequences(pairs: Sequence[tuple[Stroke, Stroke]], n_points: int = 48) -> SequenceBatch:
    """``[(基准中心线, 真实笔画), ...]`` → 序列训练批次。"""
    features: list[np.ndarray] = []
    deltas: list[np.ndarray] = []
    pressures: list[np.ndarray] = []
    pen_ups: list[np.ndarray] = []

    for base, styled in pairs:
        center = resample_n(base.points, n_points)
        actual = resample_n(styled.points, n_points)
        feat = np.zeros((n_points - 1, FEATURE_DIM), dtype=np.float32)
        delta = np.zeros((n_points - 1, 2), dtype=np.float32)
        pressure = np.zeros((n_points - 1,), dtype=np.float32)
        pen_up = np.zeros((n_points - 1,), dtype=np.float32)
        for i in range(n_points - 1):
            prev = actual[i]
            nxt = actual[i + 1]
            feat[i] = [
                center[i + 1].x,
                center[i + 1].y,
                i / (n_points - 1),
                prev.x,
                prev.y,
                prev.pressure,
                0.0,
            ]
            delta[i] = [(nxt.x - prev.x) * DELTA_SCALE, (nxt.y - prev.y) * DELTA_SCALE]
            pressure[i] = nxt.pressure
            pen_up[i] = 1.0 if i == n_points - 2 else 0.0
        features.append(feat)
        deltas.append(delta)
        pressures.append(pressure)
        pen_ups.append(pen_up)

    return SequenceBatch(
        features=np.stack(features),
        delta=np.stack(deltas),
        pressure=np.stack(pressures),
        pen_up=np.stack(pen_ups),
    )


class PenSequenceModel(nn.Module):
    """GRU + MDN：输入中心线与上一笔尖状态，输出下一个笔尖动作。"""

    def __init__(self, in_dim: int = FEATURE_DIM, hidden: int = 128, mixtures: int = 5):
        super().__init__()
        self.mixtures = mixtures
        self.gru = nn.GRU(in_dim, hidden, batch_first=True)
        self.head = nn.Linear(hidden, mixtures * 5 + 3)

    def forward(self, x: torch.Tensor, hidden: torch.Tensor | None = None):
        out, hidden = self.gru(x, hidden)
        o = self.head(out)
        k = self.mixtures
        pi = F.softmax(o[..., :k], dim=-1)
        mu = o[..., k : 3 * k].reshape(*x.shape[:-1], k, 2)
        sigma = F.softplus(o[..., 3 * k : 5 * k]).reshape(*x.shape[:-1], k, 2) + _EPS
        pen_logit = o[..., 5 * k]
        p_mu = torch.sigmoid(o[..., 5 * k + 1])
        p_sigma = F.softplus(o[..., 5 * k + 2]) + _EPS
        return pi, mu, sigma, pen_logit, p_mu, p_sigma, hidden


def train_sequences(
    batch: SequenceBatch,
    config: SequenceConfig,
    progress: Callable[[int, float], None] | None = None,
) -> tuple[PenSequenceModel, list[float]]:
    torch.manual_seed(config.seed)
    device = torch.device(config.device)
    model = PenSequenceModel(FEATURE_DIM, config.hidden, config.mixtures).to(device)
    optimizer = torch.optim.Adam(model.parameters(), lr=config.lr)

    x = torch.from_numpy(batch.features).to(device)
    y_delta = torch.from_numpy(batch.delta).to(device)
    y_pressure = torch.from_numpy(batch.pressure).to(device)
    y_pen = torch.from_numpy(batch.pen_up).to(device)

    n = x.shape[0]
    steps = max(1, math.ceil(n / config.batch_size))
    total_steps = max(1, config.epochs * steps)
    warmup = max(1, total_steps // 20)

    def lr_lambda(step: int) -> float:
        if step < warmup:
            return step / warmup
        p = (step - warmup) / max(1, total_steps - warmup)
        return 0.5 * (1.0 + math.cos(math.pi * p))

    scheduler = torch.optim.lr_scheduler.LambdaLR(optimizer, lr_lambda)
    history: list[float] = []
    steps_per_sample = x.shape[1]

    model.train()
    for epoch in range(config.epochs):
        perm = torch.randperm(n, device=device)
        total = 0.0
        for start in range(0, n, config.batch_size):
            idx = perm[start : start + config.batch_size]
            pi, mu, sigma, pen_logit, p_mu, p_sigma, _ = model(x[idx])
            loss_xy = mdn_nll(pi, mu, sigma, y_delta[idx]).mean()
            loss_pen = F.binary_cross_entropy_with_logits(pen_logit, y_pen[idx]).mean()
            loss_p = gaussian_nll(p_mu, p_sigma, y_pressure[idx]).mean() / steps_per_sample
            loss = loss_xy + config.pen_weight * loss_pen + config.pressure_weight * loss_p
            if not math.isfinite(float(loss.item())):
                raise FloatingPointError(f"序列模型训练发散：第 {epoch} 轮")
            optimizer.zero_grad()
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 5.0)
            optimizer.step()
            scheduler.step()
            total += float(loss.item()) * len(idx)
        history.append(total / n)
        if progress is not None:
            progress(epoch, history[-1])
    return model, history


@torch.no_grad()
def generate_stroke(
    model: PenSequenceModel, centerline: Sequence[Point], sample: bool = True
) -> Stroke:
    """沿中心线生成一笔；``sample=False`` 取混合均值。"""
    model.eval()
    device = next(model.parameters()).device
    n = len(centerline)
    if n < 2:
        return Stroke([centerline[0]] if centerline else [Point(0.0, 0.0)])

    points = [Point(centerline[0].x, centerline[0].y, 0.5, 0)]
    hidden = None
    pressure = 0.5
    for i in range(n - 1):
        prev = points[-1]
        feat = torch.tensor(
            [
                centerline[i + 1].x,
                centerline[i + 1].y,
                i / (n - 1),
                prev.x,
                prev.y,
                pressure,
                0.0,
            ],
            dtype=torch.float32,
            device=device,
        ).reshape(1, 1, -1)
        pi, mu, sigma, _pen_logit, p_mu, p_sigma, hidden = model(feat, hidden)
        if sample:
            k = int(torch.multinomial(pi[0, 0], 1).item())
            dx = float(mu[0, 0, k, 0] + sigma[0, 0, k, 0] * torch.randn((), device=device))
            dy = float(mu[0, 0, k, 1] + sigma[0, 0, k, 1] * torch.randn((), device=device))
            p = float(p_mu[0, 0] + p_sigma[0, 0] * torch.randn((), device=device))
        else:
            dx = float((pi[0, 0] * mu[0, 0, :, 0]).sum())
            dy = float((pi[0, 0] * mu[0, 0, :, 1]).sum())
            p = float(p_mu[0, 0])
        points.append(
            Point(
                prev.x + dx / DELTA_SCALE,
                prev.y + dy / DELTA_SCALE,
                min(1.0, max(0.0, p)),
                (i + 1) * 10,
            )
        )
        pressure = points[-1].pressure
    return Stroke(points)
