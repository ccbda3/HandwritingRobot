"""混合密度网络（MDN）的损失与采样。

连续量（残差 dx/dy、笔压）用高斯；残差用 K 个高斯混合，
因为同一个位置在不同次书写里会有不同的偏移——单一高斯会把它们平均掉。
"""

from __future__ import annotations

import math

import torch

_EPS = 1e-8


def gaussian_nll(
    mu: torch.Tensor, sigma: torch.Tensor, target: torch.Tensor, dim: int = -1
) -> torch.Tensor:
    """高斯负对数似然，``dim`` 指定被求和的维度。"""
    var = sigma * sigma
    return 0.5 * (((target - mu) ** 2) / var + torch.log(2.0 * math.pi * var)).sum(dim=dim)


def mdn_nll(
    pi: torch.Tensor, mu: torch.Tensor, sigma: torch.Tensor, target: torch.Tensor
) -> torch.Tensor:
    """混合高斯的负对数似然。

    ``pi``/``mu``/``sigma`` 形状为 ``(..., K)``/``(..., K, 2)``/``(..., K, 2)``，
    ``target`` 形状为 ``(..., 2)``；返回 ``(...)``。
    """
    # gaussian_nll 已是「负对数密度」，混合对数密度 = log(pi) - nll_component
    nll_component = gaussian_nll(mu, sigma, target.unsqueeze(-2), dim=-1)
    return -torch.logsumexp(torch.log(pi + _EPS) - nll_component, dim=-1)


def sample_component(pi_row: torch.Tensor) -> int:
    """按混合权重抽一个分量下标。``pi_row`` 形状 ``(K,)``。"""
    return int(torch.multinomial(pi_row, 1).item())
