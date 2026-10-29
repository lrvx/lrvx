"""pbo_cscv, deflated_sharpe and grid_neighborhood through the binding.

The C++ tests pin the arithmetic; these pin the binding's shapes, the
errors it raises for the wrong shape, and that the numbers reach Python
unchanged on the two hand-checkable cases.
"""

from __future__ import annotations

import math

import numpy as np
import pytest

import lrvx


def test_pbo_consistent_winner_is_zero_and_flipping_winner_is_one() -> None:
    consistent = np.array([[1.0, 2.0, 1.0, 2.0], [-1.0, -2.0, -1.0, -2.0]])
    out = lrvx.pbo_cscv(consistent, num_blocks=2)
    assert out["splits"] == 2
    assert out["pbo"] == 0.0
    assert abs(out["mean_oos_rank"] - 2.0 / 3.0) < 1e-12
    assert abs(out["mean_logit"] - math.log(2.0)) < 1e-12

    flipping = np.array([[1.0, 2.0, -1.0, -2.0], [-1.0, -2.0, 1.0, 2.0]])
    assert lrvx.pbo_cscv(flipping, num_blocks=2)["pbo"] == 1.0


def test_pbo_default_blocks_and_unsplittable_input() -> None:
    rng = np.random.default_rng(3)
    rets = rng.standard_normal((10, 256)) * 0.01
    out = lrvx.pbo_cscv(rets)
    assert out["splits"] == 12870  # C(16, 8)
    assert 0.0 <= out["pbo"] <= 1.0
    assert math.isnan(lrvx.pbo_cscv(rets, num_blocks=7)["pbo"])
    with pytest.raises(lrvx.LrvxError):
        lrvx.pbo_cscv(rets[0])  # 1-D
    with pytest.raises(lrvx.LrvxError):
        lrvx.pbo_cscv(np.empty((0, 5)))


def test_deflated_sharpe_without_trials_is_the_probabilistic_sharpe_ratio() -> None:
    rng = np.random.default_rng(21)
    r = rng.standard_normal(500) * 0.01 + 0.002
    out = lrvx.deflated_sharpe(r)
    mean = r.mean()
    sr = mean / r.std(ddof=1)
    d = r - mean
    m2 = (d**2).mean()
    skew = (d**3).mean() / m2**1.5
    kurt = (d**4).mean() / m2**2
    assert abs(out["sharpe"] - sr) < 1e-12
    assert abs(out["skewness"] - skew) < 1e-12
    assert abs(out["kurtosis"] - kurt) < 1e-12
    assert out["expected_max_sharpe"] == 0.0
    z = sr * math.sqrt(499) / math.sqrt(1 - skew * sr + (kurt - 1) / 4 * sr * sr)
    assert abs(out["dsr"] - 0.5 * math.erfc(-z / math.sqrt(2))) < 1e-12


def test_deflated_sharpe_more_trials_means_a_higher_bar() -> None:
    rng = np.random.default_rng(5)
    r = rng.standard_normal(300) * 0.01 + 0.001
    ten = lrvx.deflated_sharpe(r, trial_sharpes=np.linspace(-0.15, 0.15, 10))
    thousand = lrvx.deflated_sharpe(r, trial_sharpes=np.linspace(-0.15, 0.15, 1000))
    assert ten["expected_max_sharpe"] > 0.0
    assert thousand["expected_max_sharpe"] > ten["expected_max_sharpe"]
    assert thousand["dsr"] < ten["dsr"]
    with pytest.raises(lrvx.LrvxError):
        lrvx.deflated_sharpe(np.zeros((2, 2)))
    assert math.isnan(lrvx.deflated_sharpe(np.array([0.01]))["sharpe"])


def test_grid_neighborhood_keeps_the_shape() -> None:
    values = np.arange(1.0, 10.0).reshape(3, 3)
    out = lrvx.grid_neighborhood(values)
    assert out["neighbor_mean"].shape == (3, 3)
    assert out["neighbor_count"].shape == (3, 3)
    assert out["neighbor_count"][1, 1] == 8
    assert out["neighbor_mean"][1, 1] == 5.0
    assert out["neighbor_count"][0, 0] == 3
    assert abs(out["neighbor_mean"][0, 0] - 11.0 / 3.0) < 1e-12

    line = np.array([1.0, 2.0, 3.0, 4.0])
    wide = lrvx.grid_neighborhood(line, radius=2)
    assert wide["neighbor_count"][0] == 2
    assert wide["neighbor_mean"][0] == 2.5

    with_nan = lrvx.grid_neighborhood(np.array([1.0, np.nan, 3.0]))
    assert with_nan["neighbor_count"][0] == 0
    assert math.isnan(with_nan["neighbor_mean"][0])
    assert with_nan["neighbor_count"][1] == 2
