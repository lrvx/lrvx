"""The record a live Runner keeps of the orders it sent to its executor.

A Modify is sent as the order it names, so the runner keeps every order that
can rest. Nothing told it when one finished, so the record only grew, and a
Modify for an id it did not hold went out as a made-up LIMIT / BUY order with
no symbol. The executor is an in-process fake: nothing leaves the process.
"""
from __future__ import annotations

import threading

import pytest

import lrvx

T0 = 1_788_220_800_000_000_000


class RecordingExecutor(lrvx.Executor):
    def __init__(self):
        super().__init__()
        self.submitted = []
        self.replaced = []
        self.canceled = []

    def submit(self, order):
        self.submitted.append(order)

    def replace(self, old_id, order):
        self.replaced.append((old_id, order))

    def cancel(self, order_id):
        self.canceled.append(order_id)


class Scripted(lrvx.Strategy):
    """Runs one step of a script per trade, on the thread that delivers it,
    and keeps what lrvx.last_error() read on that thread after each step."""

    def __init__(self, symbols, steps):
        super().__init__(symbols)
        self._steps = list(steps)
        self.results = []
        self.errors = []
        self.done = threading.Event()

    def on_trade(self, ctx, trade):
        if not self._steps:
            return
        step = self._steps.pop(0)
        self.results.append(step(self))
        self.errors.append(lrvx.last_error())
        if not self._steps:
            self.done.set()


def _run(threaded, steps, between=None):
    """`between`, when given, maps a step index to a callable run on the host
    thread after that step (it gets the runner and the strategy)."""
    reg = lrvx.SymbolRegistry()
    sym = reg.add_symbol("test", "BTCUSDT", tick_size=0.01)
    runner = lrvx.Runner(reg, lambda s: None, threaded=threaded)
    executor = RecordingExecutor()
    runner.set_executor(executor)
    strat = Scripted([sym], steps)
    runner.add_strategy(strat)
    runner.start()
    try:
        for i in range(len(steps)):
            runner.on_trade(sym, 100.0, 1.0, True, T0 + i)
            if threaded:
                # One step at a time, so the host's call lands between them.
                for _ in range(2000):
                    if len(strat.errors) > i:
                        break
                    threading.Event().wait(0.005)
                assert len(strat.errors) > i, "the strategy never ran step %d" % i
            if between and i in between:
                between[i](runner, strat)
    finally:
        runner.stop()
    return strat, executor


MODES = pytest.mark.parametrize("threaded", [False, True], ids=["sync", "threaded"])


@MODES
def test_a_closed_order_is_dropped_and_a_later_modify_is_not_sent(threaded):
    steps = [
        lambda s: s.limit_sell(101.0, 3.0),
        lambda s: s.modify_order(s.results[0], 102.0, 3.0),
        lambda s: s.modify_order(s.results[0], 103.0, 3.0),
    ]
    strat, executor = _run(
        threaded, steps,
        between={1: lambda runner, s: runner.order_closed(s.results[0])})

    order_id = strat.results[0]
    assert [o.id for o in executor.submitted] == [order_id]
    assert [old for old, _ in executor.replaced] == [order_id], \
        "the modify before the close reaches the executor, the one after does not"
    assert strat.errors[1] == ""
    assert str(order_id) in strat.errors[2]
    assert strat.errors[2].endswith("[unknown_order]")


@MODES
def test_a_modify_of_an_id_never_sent_is_not_forwarded(threaded):
    strat, executor = _run(threaded, [lambda s: s.modify_order(424242, 100.0, 1.0)])
    assert executor.replaced == []
    assert "424242" in strat.errors[0]
    assert "unknown_order" in strat.errors[0]


@MODES
def test_orders_that_cannot_rest_are_not_kept(threaded):
    steps = [
        lambda s: s.market_buy(1.0),
        lambda s: s.limit_buy(99.0, 1.0, tif="ioc"),
        lambda s: s.limit_sell(101.0, 1.0, tif="fok"),
        lambda s: s.limit_buy(99.0, 1.0),
        lambda s: s.modify_order(s.results[0], 100.0, 1.0),
        lambda s: s.modify_order(s.results[1], 100.0, 1.0),
        lambda s: s.modify_order(s.results[2], 100.0, 1.0),
        lambda s: s.modify_order(s.results[3], 98.0, 1.0),
    ]
    strat, executor = _run(threaded, steps)
    assert len(executor.submitted) == 4, "all four still reach the executor"
    assert [old for old, _ in executor.replaced] == [strat.results[3]]
    for i in (4, 5, 6):
        assert "unknown_order" in strat.errors[i]
    assert strat.errors[7] == "", "modify_order clears the reason of the one before"


@MODES
def test_order_closed_for_an_unknown_id_is_ignored(threaded):
    reg = lrvx.SymbolRegistry()
    reg.add_symbol("test", "BTCUSDT", tick_size=0.01)
    runner = lrvx.Runner(reg, lambda s: None, threaded=threaded)
    runner.set_executor(RecordingExecutor())
    runner.order_closed(1)
    runner.order_closed((7 << 32) + 1)
