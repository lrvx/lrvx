"""lrvx.exits.PercentStop, TakeProfit and TimeStop. The levels are pinned to
the same numbers as tests/test_exits_percent_time.cpp: the Python and the C++
classes are held to one answer."""

from __future__ import annotations

import math

import pytest

from lrvx.exits import PercentStop, TakeProfit, TimeStop


def test_percent_stop_long_is_entry_times_one_minus_pct() -> None:
    stop = PercentStop(5.0)
    assert not stop.armed
    assert math.isnan(stop.stop)
    assert stop.update(101.0, 1.0) is False  # nothing is watched before arm
    assert stop.pct == 5.0

    stop.arm("long", 200.0)
    assert stop.armed
    assert stop.stop == 190.0
    assert stop.update(215.0, 195.0) is False
    assert stop.update(250.0, 190.01) is False
    assert stop.stop == 190.0  # a percent stop does not move
    assert stop.update(212.0, 190.0) is True  # low == stop is a hit
    assert stop.update(212.0, 191.0) is True  # reported until disarm
    assert stop.armed

    stop.disarm()
    assert not stop.armed and math.isnan(stop.stop)
    assert stop.update(100.0, 1.0) is False


def test_percent_stop_short_is_entry_times_one_plus_pct() -> None:
    stop = PercentStop(5.0)
    stop.arm("short", 200.0)
    assert stop.stop == 210.0
    assert stop.update(209.0, 195.0) is False
    assert stop.update(209.99, 100.0) is False
    assert stop.update(210.0, 201.0) is True


def test_percent_stop_formula_is_pinned_across_bindings() -> None:
    stop = PercentStop(2.5)
    stop.arm("short", 100.0)
    assert stop.stop == 100.0 * (1.0 + 2.5 / 100.0) == 102.49999999999999
    stop.arm("long", 100.0)
    assert stop.stop == 97.5


@pytest.mark.parametrize("cls", [PercentStop, TakeProfit])
@pytest.mark.parametrize("pct", [0.0, -1.0, math.nan])
def test_non_positive_pct_is_rejected(cls, pct) -> None:
    with pytest.raises(ValueError):
        cls(pct)


@pytest.mark.parametrize("cls", [PercentStop, TakeProfit])
def test_an_unknown_side_is_rejected(cls) -> None:
    rule = cls(5.0)
    with pytest.raises(ValueError):
        rule.arm("buy", 100.0)
    assert not rule.armed


def test_take_profit_long_is_entry_times_one_plus_pct() -> None:
    tp = TakeProfit(25.0)
    assert not tp.armed and math.isnan(tp.target)
    assert tp.update(1000.0, 1.0) is False
    assert tp.pct == 25.0
    tp.arm("long", 200.0)
    assert tp.target == 250.0
    assert tp.update(249.0, 190.0) is False
    assert tp.update(249.99, 1.0) is False
    assert tp.update(250.0, 205.0) is True  # high == target is a hit
    assert tp.update(240.0, 205.0) is True  # reported until disarm
    tp.disarm()
    assert not tp.armed and math.isnan(tp.target)
    assert tp.update(1000.0, 1.0) is False


def test_take_profit_short_is_entry_times_one_minus_pct() -> None:
    tp = TakeProfit(25.0)
    tp.arm("short", 200.0)
    assert tp.target == 150.0
    assert tp.update(195.0, 151.0) is False
    assert tp.update(1000.0, 150.01) is False
    assert tp.update(190.0, 150.0) is True


def test_time_stop_fires_on_the_max_bars_th_update() -> None:
    ts = TimeStop(3)
    assert ts.max_bars == 3
    assert not ts.armed
    assert ts.update() is False
    assert ts.bars == 0
    ts.arm()
    assert ts.armed and ts.bars == 0
    assert [ts.update() for _ in range(4)] == [False, False, True, True]
    assert ts.bars == 4
    assert ts.armed  # a hit does not disarm
    ts.disarm()
    assert not ts.armed and ts.bars == 0
    assert ts.update() is False
    ts.arm()
    assert [ts.update() for _ in range(3)] == [False, False, True]


def test_time_stop_arm_while_armed_restarts_and_one_bar_fires_at_once() -> None:
    ts = TimeStop(2)
    ts.arm()
    assert ts.update() is False
    ts.arm()
    assert ts.bars == 0
    assert [ts.update(), ts.update()] == [False, True]
    one = TimeStop(1)
    one.arm()
    assert one.update() is True


@pytest.mark.parametrize("bad", [0, -1])
def test_time_stop_rejects_fewer_than_one_bar(bad) -> None:
    with pytest.raises(ValueError):
        TimeStop(bad)


def test_exits_exports_the_new_rules() -> None:
    import lrvx.exits as exits

    for name in ("PercentStop", "TakeProfit", "TimeStop"):
        assert name in exits.__all__
