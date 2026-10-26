"""第 3 层序列模型骨架测试（需要 torch）。"""

from __future__ import annotations

import numpy as np
import pytest

torch = pytest.importorskip("torch")

from handwriting.model.data import resample_n  # noqa: E402
from handwriting.model.sequence import (  # noqa: E402
    FEATURE_DIM,
    SequenceConfig,
    build_sequences,
    generate_stroke,
    train_sequences,
)
from handwriting.model.synthetic import (  # noqa: E402
    make_straight_stroke,
    synthetic_pairs,
)


def _batch(count: int = 64, n: int = 16, seed: int = 0):
    return build_sequences(synthetic_pairs(count=count, n_points=n, seed=seed), n_points=n)


def _config() -> SequenceConfig:
    return SequenceConfig(
        n_points=16, hidden=32, mixtures=3, epochs=25, batch_size=16, lr=3e-3, seed=0
    )


def test_build_sequences_shapes() -> None:
    batch = _batch()
    assert batch.features.shape == (64, 15, FEATURE_DIM)
    assert batch.delta.shape == (64, 15, 2)
    assert batch.pressure.shape == (64, 15)
    assert batch.pen_up.shape == (64, 15)
    assert float(batch.pen_up[:, -1].min()) == 1.0


def test_train_decreases_loss() -> None:
    batch = _batch()
    _, history = train_sequences(batch, _config())
    assert len(history) == 25
    assert history[-1] < history[0]


def test_generate_valid_stroke() -> None:
    batch = _batch()
    model, _ = train_sequences(batch, _config())
    centerline = resample_n(make_straight_stroke(16).points, 16)
    stroke = generate_stroke(model, centerline, sample=False)
    assert len(stroke.points) == 16
    assert all(0.0 <= p.pressure <= 1.0 for p in stroke.points)
    assert np.isfinite([p.x for p in stroke.points]).all()
    assert np.isfinite([p.y for p in stroke.points]).all()
