"""A live Runner hands the executor a Modify as the order it modifies.

The executor is an in-process fake: nothing leaves the process. The replace
call has to carry the id, symbol, side and type of the order the strategy
named, with the new price and quantity, in both runner modes.
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
        self.replaced_event = threading.Event()

    def submit(self, order):
        self.submitted.append(order)

    def replace(self, old_id, order):
        self.replaced.append((old_id, order))
        self.replaced_event.set()

    def cancel(self, order_id):
        self.canceled.append(order_id)


class PlaceThenModify(lrvx.Strategy):
    """Sends one order on the first trade and modifies it on the second."""

    def __init__(self, symbols, place, new_price, new_qty):
        super().__init__(symbols)
        self._place = place
        self._new_price = new_price
        self._new_qty = new_qty
        self._seen = 0
        self.order_id = 0

    def on_trade(self, ctx, trade):
        self._seen += 1
        if self._seen == 1:
            self.order_id = self._place(self)
        elif self._seen == 2:
            self.modify_order(self.order_id, self._new_price, self._new_qty)


def _run(threaded, place, new_price, new_qty):
    reg = lrvx.SymbolRegistry()
    reg.add_symbol("test", "ETHUSDT", tick_size=0.01)
    sym = reg.add_symbol("test", "BTCUSDT", tick_size=0.01)
    assert int(sym) != 0

    signals = []
    runner = lrvx.Runner(reg, signals.append, threaded=threaded)
    executor = RecordingExecutor()
    runner.set_executor(executor)
    strat = PlaceThenModify([sym], place, new_price, new_qty)
    runner.add_strategy(strat)
    runner.start()
    try:
        runner.on_trade(sym, 100.0, 1.0, True, T0)
        runner.on_trade(sym, 100.0, 1.0, True, T0 + 1)
        if threaded:
            assert executor.replaced_event.wait(10.0), "the modify never reached the executor"
    finally:
        runner.stop()
    return int(sym), strat, executor


CASES = [
    pytest.param(lambda s: s.limit_sell(101.0, 3.0), 102.0, 2.0,
                 dict(side="sell", order_type="limit", price=102.0, trigger_price=0.0),
                 id="limit-sell"),
    pytest.param(lambda s: s.limit_buy(99.0, 3.0), 98.5, 1.0,
                 dict(side="buy", order_type="limit", price=98.5, trigger_price=0.0),
                 id="limit-buy"),
    pytest.param(lambda s: s.stop_limit("sell", 99.0, 98.9, 1.0), 98.8, 1.0,
                 dict(side="sell", order_type="stop_limit", price=98.8, trigger_price=99.0),
                 id="stop-limit"),
    pytest.param(lambda s: s.stop_market("sell", 99.0, 1.0), 99.5, 2.0,
                 dict(side="sell", order_type="stop_market", price=0.0, trigger_price=99.5),
                 id="stop-market"),
]


@pytest.mark.parametrize("threaded", [False, True], ids=["sync", "threaded"])
@pytest.mark.parametrize("place,new_price,new_qty,want", CASES)
def test_modify_reaches_the_executor_as_the_order_it_names(threaded, place, new_price, new_qty,
                                                            want):
    sym, strat, executor = _run(threaded, place, new_price, new_qty)

    assert strat.order_id != 0
    assert [o.id for o in executor.submitted] == [strat.order_id]
    assert len(executor.replaced) == 1
    old_id, order = executor.replaced[0]
    assert old_id == strat.order_id
    assert order.id == strat.order_id
    assert order.symbol == sym
    assert order.side == want["side"]
    assert order.order_type == want["order_type"]
    assert order.price == pytest.approx(want["price"])
    assert order.trigger_price == pytest.approx(want["trigger_price"])
    assert order.quantity == pytest.approx(new_qty)
    assert order.filled_quantity == 0.0
