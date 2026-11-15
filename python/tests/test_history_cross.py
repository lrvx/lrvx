"""lrvx.History (a fixed-capacity ring, newest first) and
lrvx.crossed_above / lrvx.crossed_below."""

from __future__ import annotations

import math

import pytest

import lrvx


def test_history_is_newest_first_and_drops_the_oldest() -> None:
    h = lrvx.History(3)
    assert h.capacity == 3
    assert len(h) == 0 and not h.full
    with pytest.raises(IndexError):
        h[0]
    h.push(10.0)
    assert len(h) == 1 and h[0] == 10.0 and h.get(0) == 10.0
    with pytest.raises(IndexError):
        h.get(1)
    h.push(20.0)
    assert (h[0], h[1]) == (20.0, 10.0)
    assert not h.full
    h.push(30.0)
    assert h.full and len(h) == 3 and h[2] == 10.0
    h.push(40.0)
    assert len(h) == 3
    assert [h[i] for i in range(3)] == [40.0, 30.0, 20.0]
    with pytest.raises(IndexError):
        h[3]  # index == size
    for v in (50.0, 60.0, 70.0, 80.0):
        h.push(v)
    assert [h.get(i) for i in range(3)] == [80.0, 70.0, 60.0]


def test_history_capacity_one_and_nan() -> None:
    one = lrvx.History(1)
    one.push(1.0)
    one.push(2.0)
    assert len(one) == 1 and one[0] == 2.0 and one.full
    h = lrvx.History(2)
    h.push(math.nan)
    h.push(5.0)
    assert len(h) == 2 and h[0] == 5.0 and math.isnan(h[1])


@pytest.mark.parametrize("bad", [0, -1])
def test_history_rejects_capacity_below_one(bad) -> None:
    with pytest.raises(ValueError):
        lrvx.History(bad)


def test_crossed_above_edges() -> None:
    up = lrvx.crossed_above
    assert up(1.0, 3.0, 2.0, 2.0) is True
    assert up(3.0, 4.0, 2.0, 2.0) is False  # already above
    assert up(1.0, 2.0, 2.0, 2.0) is False  # touching is not a cross
    assert up(2.0, 2.5, 2.0, 2.0) is True  # leaving upward from equal is a cross
    assert up(2.0, 2.0, 2.0, 2.0) is False
    assert up(3.0, 1.0, 2.0, 2.0) is False
    assert up(10.0, 12.0, 11.0, 11.5) is True
    assert up(10.0, 12.0, 11.0, 13.0) is False
    assert up(10.0, 10.0, 10.5, 9.5) is True
    assert up(12.0, 13.0, 11.0, 11.5) is False


def test_crossed_below_edges() -> None:
    down = lrvx.crossed_below
    assert down(3.0, 1.0, 2.0, 2.0) is True
    assert down(1.0, 0.5, 2.0, 2.0) is False
    assert down(3.0, 2.0, 2.0, 2.0) is False
    assert down(2.0, 1.5, 2.0, 2.0) is True
    assert down(2.0, 2.0, 2.0, 2.0) is False
    assert down(1.0, 3.0, 2.0, 2.0) is False
    assert down(12.0, 10.0, 11.5, 11.0) is True
    assert down(12.0, 10.0, 11.0, 9.0) is False


@pytest.mark.parametrize("missing", [None, math.nan])
@pytest.mark.parametrize("at", range(4))
def test_none_or_nan_is_no_cross(missing, at) -> None:
    a = [1.0, 3.0, 2.0, 2.0]
    a[at] = missing
    assert lrvx.crossed_above(*a) is False
    b = [3.0, 1.0, 2.0, 2.0]
    b[at] = missing
    assert lrvx.crossed_below(*b) is False


def test_crossings_follow_an_indicator_over_a_series() -> None:
    """A price against a level, the way a strategy calls it bar by bar."""
    prices = [9.0, 10.0, 11.0, 10.0, 9.0, 10.0, 10.0, 12.0]
    level = 10.0
    ups, downs = [], []
    for i in range(1, len(prices)):
        if lrvx.crossed_above(prices[i - 1], prices[i], level, level):
            ups.append(i)
        if lrvx.crossed_below(prices[i - 1], prices[i], level, level):
            downs.append(i)
    # 9 -> 10 touches (no), 10 -> 11 leaves upward from equal (yes),
    # 11 -> 10 touches (no), 10 -> 9 leaves downward (yes),
    # 9 -> 10 touches, 10 -> 10 stays, 10 -> 12 leaves upward (yes).
    assert ups == [2, 7]
    assert downs == [4]
