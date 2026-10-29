"""lrvx.exits pins the same scenario as tests/test_exits.cpp: the Python and
the C++ classes are held to one set of levels."""

from __future__ import annotations

import math

import pytest

from lrvx.exits import AtrStop, BreakEvenTrail, ChandelierExit

LONG_BARS = [
    (103.0, 99.0, 2.0),
    (106.0, 102.0, 2.0),
    (110.0, 105.0, 2.5),
    (108.0, 104.0, 2.5),
    (105.0, 101.5, 2.5),
    (103.0, 97.0, 3.0),
]


def test_atr_stop_is_frozen_at_entry_minus_multiple_of_atr() -> None:
    stop = AtrStop(2.0)
    assert not stop.armed
    assert stop.update(101.0, 99.0, 2.0) is False
    stop.arm("long", 100.0, 2.0)
    assert stop.stop == 96.0
    hit = False
    for high, low, atr in LONG_BARS:
        hit = stop.update(high, low, atr)
        assert stop.stop == 96.0
        if hit:
            break
    assert not hit  # the lowest low, 97, stays above 96

    stop.arm("short", 100.0, 2.0)
    assert stop.stop == 104.0
    assert stop.update(103.0, 99.0, 2.0) is False
    assert stop.update(104.0, 101.0, 2.0) is True
    assert stop.update(104.0, 101.0, 2.0) is True  # reported until disarm
    stop.disarm()
    assert not stop.armed and math.isnan(stop.stop)
    with pytest.raises(ValueError):
        stop.arm("buy", 100.0, 2.0)


def test_chandelier_hangs_below_the_highest_high_and_only_rises() -> None:
    rule = ChandelierExit(0, 3.0)
    rule.arm("long", 100.0, 2.0)
    assert rule.stop == 94.0
    assert rule.update(103.0, 99.0, 2.0) is False
    assert rule.stop == 97.0
    assert rule.update(106.0, 102.0, 2.0) is False
    assert rule.stop == 100.0
    assert rule.update(110.0, 105.0, 2.5) is False
    assert rule.stop == 102.5
    assert rule.update(108.0, 104.0, 2.5) is False
    assert rule.stop == 102.5
    assert rule.update(105.0, 101.5, 2.5) is True
    assert rule.stop == 102.5


def test_chandelier_lookback_drops_old_highs_but_the_stop_never_falls() -> None:
    rule = ChandelierExit(2, 3.0)
    rule.arm("long", 100.0, 2.0)
    for high, low, atr in LONG_BARS[:4]:
        assert rule.update(high, low, atr) is False
    assert rule.stop == 102.5
    assert rule.update(105.0, 101.5, 2.5) is True
    assert rule.stop == 102.5


def test_chandelier_short_mirrors_the_long() -> None:
    rule = ChandelierExit(0, 3.0)
    rule.arm("short", 100.0, 2.0)
    assert rule.stop == 106.0
    assert rule.update(101.0, 97.0, 2.0) is False
    assert rule.stop == 103.0
    assert rule.update(99.0, 94.0, 2.0) is False
    assert rule.stop == 100.0
    assert rule.update(100.5, 96.0, 2.0) is True


def test_break_even_trail_moves_to_entry_on_the_trigger_then_trails() -> None:
    rule = BreakEvenTrail(2.0, 3.0, 1.5)
    rule.arm("long", 100.0, 2.0)
    assert rule.stop == 96.0 and not rule.at_break_even
    assert rule.update(103.0, 99.0, 2.0) is False
    assert rule.stop == 96.0 and not rule.at_break_even
    assert rule.update(106.0, 102.0, 2.0) is False
    assert rule.at_break_even and rule.stop == 103.0
    assert rule.update(110.0, 105.0, 2.5) is False
    assert rule.stop == 106.25
    assert rule.update(108.0, 104.0, 2.5) is True
    assert rule.stop == 106.25


def test_break_even_trail_stops_at_entry_when_the_trail_would_be_below_it() -> None:
    rule = BreakEvenTrail(2.0, 1.0, 4.0)
    rule.arm("long", 100.0, 2.0)
    assert rule.update(102.5, 99.5, 2.0) is False
    assert rule.at_break_even and rule.stop == 100.0
    assert rule.update(101.0, 100.0, 2.0) is True


def test_break_even_trail_short_mirrors_the_long() -> None:
    rule = BreakEvenTrail(2.0, 3.0, 1.5)
    rule.arm("short", 100.0, 2.0)
    assert rule.stop == 104.0
    assert rule.update(101.0, 97.0, 2.0) is False
    assert not rule.at_break_even
    assert rule.update(98.0, 94.0, 2.0) is False
    assert rule.at_break_even and rule.stop == 97.0
    assert rule.update(97.5, 95.0, 2.0) is True
