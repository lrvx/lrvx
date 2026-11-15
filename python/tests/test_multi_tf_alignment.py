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


# A bar type outside 0 (Time) .. 6 (BpsRange) is a mistake in the caller's
# code, not a timeframe that has not closed yet. The other bindings refuse it
# (TypeError in Node and QuickJS, ValueError in Codon); here it is the
# binding's structured argument error, LrvxError E_VAL_002, naming the method,
# the argument and the value. 7..255 used to read None / [] as if no bar of
# that timeframe had closed.
_BAD_BAR_TYPES = (7, 8, 255)


def _assert_refused(call, method: str, arg: str, value: int) -> None:
    try:
        got = call()
    except lrvx.LrvxError as e:
        assert e.code == "E_VAL_002", f"{method}: code {e.code!r}"
        assert method in e.message, f"message does not name {method}: {e.message!r}"
        assert arg in e.message, f"message does not name {arg}: {e.message!r}"
        assert str(value) in e.message, f"message does not name {value}: {e.message!r}"
        return
    raise AssertionError(f"{method}({arg}={value}) returned {got!r} instead of raising")


def test_bar_type_outside_0_to_6_is_refused() -> None:
    runner, strat, sym, push, _closes = _ring_rig()
    try:
        push(0, 3000.0)
        for bt in _BAD_BAR_TYPES:
            _assert_refused(lambda: strat.last_closed_bar(sym.id, bt, MIN),
                            "last_closed_bar", "bar_type", bt)
            _assert_refused(lambda: strat.last_n_closed_bars(sym.id, bt, MIN, 5),
                            "last_n_closed_bars", "bar_type", bt)
        # The valid ends of the range still read.
        assert strat.last_closed_bar(sym.id, 0, MIN)["close"] == 3000.0
        assert strat.last_closed_bar(sym.id, 6, MIN) is None
        assert strat.last_n_closed_bars(sym.id, 6, MIN, 5) == []
    finally:
        runner.stop()


def test_bar_type_is_checked_before_the_strategy_is_attached() -> None:
    """The argument is wrong whether or not a runner holds the strategy, so a
    detached strategy refuses it too rather than reading None."""
    s = lrvx.Strategy(symbols=[1])
    _assert_refused(lambda: s.last_closed_bar(1, 7, MIN), "last_closed_bar", "bar_type", 7)
    _assert_refused(lambda: s.last_n_closed_bars(1, 7, MIN, 3), "last_n_closed_bars",
                    "bar_type", 7)


# The C API and every other binding take the capacity as a uint32: Codon
# raises ValueError and Node / QuickJS a TypeError above 4294967295. Python
# took any size_t and stored it as is, so 2**32 read back 4294967296 here and
# would be cut to 0 (taken as 1) by the C API.
_CAP_MAX = 4294967295


def test_capacity_above_uint32_is_refused_and_leaves_the_capacity() -> None:
    runner, strat, _sym, _push, _closes = _ring_rig()
    try:
        strat.set_bar_ring_capacity(5)
        for n in (_CAP_MAX + 1, 2**64 - 1):
            _assert_refused(lambda: strat.set_bar_ring_capacity(n),
                            "set_bar_ring_capacity", "capacity", n)
            assert strat.bar_ring_capacity() == 5, (
                f"after a refused set_bar_ring_capacity({n}) the capacity reads "
                f"{strat.bar_ring_capacity()}")
        strat.set_bar_ring_capacity(_CAP_MAX)
        assert strat.bar_ring_capacity() == _CAP_MAX
    finally:
        runner.stop()


def test_capacity_is_checked_before_the_strategy_is_attached() -> None:
    s = lrvx.Strategy(symbols=[1])
    _assert_refused(lambda: s.set_bar_ring_capacity(_CAP_MAX + 1),
                    "set_bar_ring_capacity", "capacity", _CAP_MAX + 1)
