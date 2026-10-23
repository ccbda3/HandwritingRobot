"""训练循环与检查点。

损失 = 残差的混合高斯 NLL + 笔压的高斯 NLL（带权重）。
学习率用 warmup + cosine 退火。
"""

from __future__ import annotations

import math
from collections.abc import Callable, Sequence
from dataclasses import asdict, dataclass
from pathlib import Path

import torch

from handwriting.model.data import FEATURE_DIM, StrokeExample, build_examples, stack
from handwriting.model.mdn import gaussian_nll, mdn_nll
from handwriting.model.net import StyleResidualNet


@dataclass(slots=True)
class TrainConfig:
    n_points: int = 48
    smooth_window: int = 5
    hidden: int = 128
    mixtures: int = 5
    epochs: int = 300
    lr: float = 1e-3
    batch_size: int = 64
    seed: int = 0
    device: str = "cpu"
    pressure_weight: float = 0.5


def default_device() -> str:
    return "cuda" if torch.cuda.is_available() else "cpu"


def _tensors(
    examples: Sequence[StrokeExample], device: torch.device
) -> tuple[torch.Tensor, torch.Tensor, torch.Tensor]:
    x, y_xy, y_p = stack(examples)
    return (
        torch.from_numpy(x).to(device),
        torch.from_numpy(y_xy).to(device),
        torch.from_numpy(y_p).to(device),
    )


def train(
    examples: Sequence[StrokeExample],
    config: TrainConfig,
    progress: Callable[[int, float], None] | None = None,
) -> tuple[StyleResidualNet, list[float]]:
    """训练模型，返回 ``(model, 每轮平均损失)``。"""
    torch.manual_seed(config.seed)
    device = torch.device(config.device)
    model = StyleResidualNet(FEATURE_DIM, config.hidden, config.mixtures).to(device)
    optimizer = torch.optim.Adam(model.parameters(), lr=config.lr)

    x, y_xy, y_p = _tensors(examples, device)
    n = x.shape[0]
    steps_per_epoch = max(1, math.ceil(n / config.batch_size))
    total_steps = max(1, config.epochs * steps_per_epoch)
    warmup = max(1, total_steps // 20)

    def lr_lambda(step: int) -> float:
        if step < warmup:
            return step / warmup
        p = (step - warmup) / max(1, total_steps - warmup)
        return 0.5 * (1.0 + math.cos(math.pi * p))

    scheduler = torch.optim.lr_scheduler.LambdaLR(optimizer, lr_lambda)

    history: list[float] = []
    model.train()
    for epoch in range(config.epochs):
        perm = torch.randperm(n, device=device)
        total = 0.0
        for start in range(0, n, config.batch_size):
            idx = perm[start : start + config.batch_size]
            pi, mu, sigma, p_mu, p_sigma = model(x[idx])
            loss_xy = mdn_nll(pi, mu, sigma, y_xy[idx]).mean()
            loss_p = gaussian_nll(p_mu, p_sigma, y_p[idx]).mean() / config.n_points
            loss = loss_xy + config.pressure_weight * loss_p
            if not math.isfinite(float(loss.item())):
                raise FloatingPointError(f"训练发散：第 {epoch} 轮损失为 {loss.item()}")
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


def save_checkpoint(
    model: StyleResidualNet,
    config: TrainConfig,
    path: str | Path,
    meta: dict | None = None,
) -> Path:
    out = Path(path)
    out.parent.mkdir(parents=True, exist_ok=True)
    torch.save(
        {"state_dict": model.state_dict(), "config": asdict(config), "meta": meta or {}},
        out,
    )
    return out


def load_checkpoint(
    path: str | Path, device: str = "cpu"
) -> tuple[StyleResidualNet, TrainConfig, dict]:
    ckpt = torch.load(path, map_location=device, weights_only=False)
    config = TrainConfig(**ckpt["config"])
    model = StyleResidualNet(FEATURE_DIM, config.hidden, config.mixtures)
    model.load_state_dict(ckpt["state_dict"])
    model.to(device).eval()
    return model, config, ckpt.get("meta", {})


def train_synthetic(
    config: TrainConfig | None = None, count: int = 240, **style_kwargs
) -> tuple[StyleResidualNet, list[float]]:
    """用合成笔迹训练，便于没有数位板时自测。"""
    from handwriting.model.synthetic import synthetic_pairs

    cfg = config or TrainConfig()
    pairs = synthetic_pairs(count=count, n_points=cfg.n_points, seed=cfg.seed, **style_kwargs)
    strokes = [styled for _, styled in pairs]
    centers = [base for base, _ in pairs]
    examples = build_examples(strokes, cfg.n_points, cfg.smooth_window, centerlines=centers)
    return train(examples, cfg)
