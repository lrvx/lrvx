"""lrvx.Highest / lrvx.Lowest: the max / min of the last n inputs, the
current one included, with SMA's streaming contract (value None until ready,
a NaN poisons the windows it is in), and the batch lrvx.highest /
lrvx.lowest."""

from __future__ import annotations

import math

import numpy as np
import pytest

import lrvx

NAN = math.nan
INPUT = [5, 9, 4, 3, 7, 7, 2, 8, NAN, 1, 6, 0.5, 6]
HIGHEST = [None, None, 9, 9, 7, 7, 7, 8, None, None, None, 6, 6]
LOWEST = [None, None, 4, 3, 3, 3, 2, 2, None, None, None, 0.5, 0.5]


@pytest.mark.parametrize("cls,want", [(lambda n: lrvx.Highest(n), HIGHEST),
                                      (lambda n: lrvx.Lowest(n), LOWEST)],
                         ids=["Highest", "Lowest"])
def test_streaming_warm_up_drop_out_equal_values_nan_and_reset(cls, want) -> None:
    ind = cls(3)
    assert ind.value is None and not ind.ready and ind.count == 0
    for i, (x, w) in enumerate(zip(INPUT, want)):
        got = ind.update(float(x))
        assert got == w, f"step {i}: update returned {got!r}, want {w!r}"
        assert ind.value == w, f"step {i}"
        assert ind.ready == (w is not None), f"step {i}"
        assert ind.count == i + 1

    ind.reset()
    assert ind.value is None and not ind.ready and ind.count == 0
    ind.update(1.0)
    ind.update(2.0)
    assert not ind.ready
    ind.update(3.0)
    assert ind.ready


@pytest.mark.parametrize("name,want", [("Highest", HIGHEST), ("Lowest", LOWEST)])
def test_batch_compute_and_function_equal_the_stream(name, want) -> None:
    arr = np.array(INPUT, dtype=np.float64)
    out = getattr(lrvx, name)(3).compute(arr)
    fn = getattr(lrvx, name.lower())(arr, 3)
    for i, w in enumerate(want):
        for label, got in (("compute", out[i]), ("function", fn[i])):
            if w is None:
                assert math.isnan(got), f"{label}[{i}] = {got}"
            else:
                assert got == w, f"{label}[{i}] = {got}, want {w}"


@pytest.mark.parametrize("name,pick", [("Highest", max), ("Lowest", min)])
@pytest.mark.parametrize("period", [1, 2, 5, 20])
def test_streaming_matches_compute_and_a_naive_window(name, pick, period) -> None:
    rng = np.random.default_rng(11)
    x = 100.0 + np.cumsum(rng.normal(size=600))
    x[::13] = np.roll(x, 1)[::13]  # repeated values
    x[[50, 51, 400]] = np.nan
    batch = getattr(lrvx, name)(period).compute(x)
    stream = getattr(lrvx, name)(period)
    for i, v in enumerate(x):
        got = stream.update(float(v))
        win = x[max(0, i + 1 - period): i + 1]
        if i + 1 < period or np.isnan(win).any():
            assert got is None and math.isnan(batch[i]), f"step {i}"
        else:
            assert got == pick(win) == batch[i], f"step {i}"


@pytest.mark.parametrize("name", ["Highest", "Lowest"])
def test_period_zero_is_rejected_like_sma(name) -> None:
    with pytest.raises(lrvx.LrvxError):
        getattr(lrvx, name)(0)
    with pytest.raises(lrvx.LrvxError):
        getattr(lrvx, name.lower())(np.array([1.0, 2.0]), 0)


def test_listed_as_indicators() -> None:
    names = lrvx.list_indicators()
    assert "Highest" in names and "Lowest" in names


@pytest.mark.parametrize("name,before,after,want", [
    ("Highest", [50, 90, 70, 80, NAN], [1, 3, 2, 1, 0.5, 0.25], [None, None, 3, 3, 2, 1]),
    ("Lowest", [-50, -90, -70, -80, NAN], [10, 8, 9, 10, 11, 12], [None, None, 8, 8, 9, 10]),
])
def test_reset_forgets_the_old_values(name, before, after, want) -> None:
    """The extremes, the warm-up and the NaN of the stream before reset() do
    not reach the stream after it."""
    ind = getattr(lrvx, name)(3)
    for x in before:
        ind.update(float(x))
    ind.reset()
    got = [ind.update(float(x)) for x in after]
    assert got == want
    assert ind.count == len(after) and ind.ready
