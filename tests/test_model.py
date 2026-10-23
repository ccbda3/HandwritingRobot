"""风格模型 v0 的测试（需要 torch，缺失时自动跳过）。"""

from __future__ import annotations

import numpy as np
import pytest

torch = pytest.importorskip("torch")

from handwriting.model.data import FEATURE_DIM, build_examples  # noqa: E402
from handwriting.model.generate import stylize_stroke_set  # noqa: E402
from handwriting.model.synthetic import (  # noqa: E402
    make_straight_stroke,
    synthetic_pairs,
)
from handwriting.model.train import (  # noqa: E402
    TrainConfig,
    load_checkpoint,
    save_checkpoint,
    train,
)
from handwriting.stroke import Point, Stroke, StrokeSet  # noqa: E402


def _config(**overrides) -> TrainConfig:
    base = {
        "n_points": 24,
        "hidden": 32,
        "mixtures": 3,
        "epochs": 30,
        "batch_size": 16,
        "lr": 3e-3,
        "seed": 0,
        "device": "cpu",
    }
    base.update(overrides)
    return TrainConfig(**base)


def _examples(count: int, seed: int, **style) -> list:
    pairs = synthetic_pairs(count=count, n_points=24, seed=seed, **style)
    return build_examples(
        [styled for _, styled in pairs],
        24,
        5,
        centerlines=[base for base, _ in pairs],
    )


def test_build_examples_with_centerline() -> None:
    examples = _examples(6, 0)
    assert len(examples) == 6
    assert examples[0].features.shape == (24, FEATURE_DIM)
    assert examples[0].residual.shape == (24, 2)
    assert examples[0].pressure.shape == (24,)


def test_centerline_count_mismatch() -> None:
    pairs = synthetic_pairs(count=3, n_points=24, seed=0)
    with pytest.raises(ValueError):
        build_examples([styled for _, styled in pairs], 24, 5, centerlines=[])


def test_train_decreases_loss() -> None:
    _, history = train(_examples(96, 1), _config())
    assert len(history) == 30
    assert history[-1] < history[0]


def test_generate_valid_stroke_set() -> None:
    cfg = _config()
    model, _ = train(_examples(96, 2), cfg)
    target = StrokeSet([make_straight_stroke(24)], "一")
    out = stylize_stroke_set(model, cfg, target)
    assert out.stroke_count == 1
    assert len(out.strokes[0]) == 24
    assert all(0.0 <= p.pressure <= 1.0 for p in out.strokes[0])


def test_learns_slant() -> None:
    cfg = _config(epochs=60)
    model, _ = train(_examples(160, 3, slant=0.4), cfg)
    vertical = Stroke([Point(0.5, 0.1 + 0.8 * i / 23, 0.5, i * 10) for i in range(24)])
    out = stylize_stroke_set(model, cfg, StrokeSet([vertical], "丨"))
    points = out.strokes[0].points
    ys = np.array([p.y for p in points])
    xs = np.array([p.x for p in points])
    top = xs[ys < np.median(ys)].mean()
    bottom = xs[ys > np.median(ys)].mean()
    assert bottom - top > 0.03


def test_checkpoint_roundtrip(tmp_path) -> None:
    cfg = _config(epochs=5)
    model, _ = train(_examples(32, 4), cfg)
    path = tmp_path / "style.pt"
    save_checkpoint(model, cfg, path, meta={"source": "test"})
    _, loaded_cfg, meta = load_checkpoint(path)
    assert loaded_cfg.n_points == 24
    assert loaded_cfg.mixtures == 3
    assert meta["source"] == "test"
