"""Smoke tests for multi-TF alignment helpers exposed on the Python Strategy.

Strategy authors don't construct PyStrategyBase directly without an
engine to drive it; instead these tests exercise the wired-up surface
by running a tiny strategy through a backtest. The key assertion is
that `last_closed_bar` and `last_n_closed_bars` return the expected
state at each step.
"""

from __future__ import annotations

import lrvx


def test_strategy_class_exposes_helpers() -> None:
    s = lrvx.Strategy(symbols=[1])
    # Without an engine bridge the helpers must return None / empty / 0
    # rather than crashing. This is the cold-start contract.
    assert s.last_closed_bar(1, 0, 60_000_000_000) is None
    assert s.last_n_closed_bars(1, 0, 60_000_000_000, 3) == []
    assert s.bar_ring_capacity() == 0  # no bridge => unset


def test_helpers_documented_on_strategy_class() -> None:
    # Doc strings are used by IDE autocomplete; basic sanity check that
    # the binding emitted them.
    cls = lrvx.Strategy
    assert "last_closed_bar" in dir(cls)
    assert "last_n_closed_bars" in dir(cls)
    assert "set_bar_ring_capacity" in dir(cls)
    assert "bar_ring_capacity" in dir(cls)


MIN = 60_000_000_000


def _ring_rig():
    reg = lrvx.SymbolRegistry()
    sym = reg.add_symbol("test", "RING", tick_size=0.01)
    strat = lrvx.Strategy([sym])
    runner = lrvx.Runner(reg, lambda s: None)
    runner.add_strategy(strat)
    runner.start()

    def push(i: int, base: float) -> None:
        # Bar i closes at base + i.
        c = base + i
        runner.on_bar(sym, c - 0.5, c + 1.0, c - 1.0, c, 10.0, 5.0, i * MIN, (i + 1) * MIN,
                      0, MIN)

    def closes() -> list:
        return [b["close"] for b in strat.last_n_closed_bars(sym.id, 0, MIN, 100)]

    return runner, strat, sym, push, closes


def test_lowering_capacity_trims_a_full_ring() -> None:
    """A ring already full at the default 64 bars: after the capacity is
    lowered to 3, reads return only the newest 3, oldest first, and the ring
    stays at 3 once the next bar closes."""
    runner, strat, sym, push, closes = _ring_rig()
    try:
        assert strat.bar_ring_capacity() == 64
        for i in range(70):
            push(i, 1000.25)
        assert len(closes()) == 64

        strat.set_bar_ring_capacity(3)
        assert strat.bar_ring_capacity() == 3
        assert closes() == [1067.25, 1068.25, 1069.25]
        assert strat.last_closed_bar(sym.id, 0, MIN)["close"] == 1069.25

        push(70, 1000.25)
        assert closes() == [1068.25, 1069.25, 1070.25]
    finally:
        runner.stop()


def test_raising_capacity_keeps_existing_bars() -> None:
    runner, strat, _sym, push, closes = _ring_rig()
    try:
        strat.set_bar_ring_capacity(3)
        for i in range(5):
            push(i, 2000.5)
        strat.set_bar_ring_capacity(5)
        assert closes() == [2002.5, 2003.5, 2004.5]
        push(5, 2000.5)
        push(6, 2000.5)
        assert closes() == [2002.5, 2003.5, 2004.5, 2005.5, 2006.5]
        push(7, 2000.5)
        assert closes() == [2003.5, 2004.5, 2005.5, 2006.5, 2007.5]
    finally:
        runner.stop()
