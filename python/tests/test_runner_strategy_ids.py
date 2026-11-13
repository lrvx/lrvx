"""Strategies on one Runner hold different subscriber ids and share the
record of what went to the executor.

Order ids are (subscriber id << 32) + n. A Runner numbers its strategies in
the order they are added, so no two share an id space. In the threaded mode
every strategy has its own signal handler; a modify of an order another
strategy placed used to reach the executor as a bare limit order with no
symbol and no side. The executor is an in-process fake.
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
        self.submitted_event = threading.Event()
        self.replaced_event = threading.Event()

    def submit(self, order):
        self.submitted.append(order)
        self.submitted_event.set()

    def replace(self, old_id, order):
        self.replaced.append((old_id, order))
        self.replaced_event.set()


class Shared:
    order_id = 0


class Places(lrvx.Strategy):
    def __init__(self, symbols, shared):
        super().__init__(symbols)
        self._shared = shared
        self._seen = 0

    def on_trade(self, ctx, trade):
        self._seen += 1
        if self._seen == 1:
            self._shared.order_id = self.stop_market("sell", 99.0, 3.0)


class Manages(lrvx.Strategy):
    def __init__(self, symbols, shared):
        super().__init__(symbols)
        self._shared = shared
        self._seen = 0
        self.own_id = 0

    def on_trade(self, ctx, trade):
        self._seen += 1
        if self._seen == 1:
            self.own_id = self.limit_buy(90.0, 1.0)
        elif self._seen == 2:
            self.modify_order(self._shared.order_id, 98.5, 2.0)


def _run(threaded):
    reg = lrvx.SymbolRegistry()
    reg.add_symbol("test", "ETHUSDT", tick_size=0.01)
    sym = reg.add_symbol("test", "BTCUSDT", tick_size=0.01)
    shared = Shared()
    runner = lrvx.Runner(reg, lambda s: None, threaded=threaded)
    executor = RecordingExecutor()
    runner.set_executor(executor)
    placer = Places([sym], shared)
    manager = Manages([sym], shared)
    runner.add_strategy(placer)
    runner.add_strategy(manager)
    runner.start()
    try:
        runner.on_trade(sym, 100.0, 1.0, True, T0)
        if threaded:
            # Both strategies' first orders are out before the second trade.
            for _ in range(200):
                if len(executor.submitted) == 2:
                    break
                executor.submitted_event.wait(0.05)
                executor.submitted_event.clear()
        assert len(executor.submitted) == 2
        runner.on_trade(sym, 100.0, 1.0, True, T0 + 1)
        if threaded:
            assert executor.replaced_event.wait(10.0), "the modify never reached the executor"
    finally:
        runner.stop()
    return int(sym), shared, manager, executor


@pytest.mark.parametrize("threaded", [False, True], ids=["sync", "threaded"])
def test_two_strategies_never_share_an_order_id(threaded):
    _sym, shared, manager, _executor = _run(threaded)
    assert shared.order_id != 0 and manager.own_id != 0
    assert shared.order_id != manager.own_id
    assert shared.order_id >> 32 == 1
    assert manager.own_id >> 32 == 2
    assert shared.order_id & 0xFFFFFFFF == manager.own_id & 0xFFFFFFFF == 1


@pytest.mark.parametrize("threaded", [False, True], ids=["sync", "threaded"])
def test_a_modify_of_another_strategys_order_carries_that_order(threaded):
    sym, shared, _manager, executor = _run(threaded)
    assert len(executor.replaced) == 1
    old_id, order = executor.replaced[0]
    assert old_id == shared.order_id
    assert order.id == shared.order_id
    assert order.symbol == sym
    assert order.side == "sell"
    assert order.order_type == "stop_market"
    assert order.trigger_price == pytest.approx(98.5)
    assert order.quantity == pytest.approx(2.0)


def test_the_same_strategy_object_added_twice_gets_two_ids():
    reg = lrvx.SymbolRegistry()
    sym = reg.add_symbol("test", "BTCUSDT", tick_size=0.01)
    runner = lrvx.Runner(reg, lambda s: None)
    strat = lrvx.Strategy([sym])
    runner.add_strategy(strat)
    runner.add_strategy(strat)
