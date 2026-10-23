"""MDN 残差网络：中心线特征 → 残差分布 + 笔压分布。"""

from __future__ import annotations

import torch
from torch import nn
from torch.nn import functional as F  # noqa: N812

from handwriting.model.data import FEATURE_DIM

_EPS = 1e-3


class StyleResidualNet(nn.Module):
    """一个小型 MLP，输出混合高斯的参数与笔压参数。

    输入：``(..., FEATURE_DIM)``
    输出：``pi (..., K)``、``mu (..., K, 2)``、``sigma (..., K, 2)``、
    ``p_mu (...)``、``p_sigma (...)``。
    """

    def __init__(self, in_dim: int = FEATURE_DIM, hidden: int = 128, mixtures: int = 5) -> None:
        super().__init__()
        self.mixtures = mixtures
        self.trunk = nn.Sequential(
            nn.Linear(in_dim, hidden),
            nn.Tanh(),
            nn.Linear(hidden, hidden),
            nn.Tanh(),
        )
        self.out = nn.Linear(hidden, mixtures * 5 + 2)

    def forward(
        self, x: torch.Tensor
    ) -> tuple[torch.Tensor, torch.Tensor, torch.Tensor, torch.Tensor, torch.Tensor]:
        h = self.trunk(x)
        o = self.out(h)
        k = self.mixtures

        pi = F.softmax(o[..., :k], dim=-1)
        mu = o[..., k : 3 * k].reshape(*x.shape[:-1], k, 2)
        sigma = F.softplus(o[..., 3 * k : 5 * k]).reshape(*x.shape[:-1], k, 2) + _EPS
        p_mu = torch.sigmoid(o[..., 5 * k])
        p_sigma = F.softplus(o[..., 5 * k + 1]) + _EPS
        return pi, mu, sigma, p_mu, p_sigma
