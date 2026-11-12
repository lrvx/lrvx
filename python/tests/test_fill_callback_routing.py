"""Which callback an order event reaches.

A fill (``FILLED`` or ``PARTIALLY_FILLED``) reaches ``on_fill`` and only
``on_fill``; ``on_order_update`` receives the other lifecycle statuses and
never a fill. The backtest runner and the paper broker route the same way.
"""
from __future__ import annotations

import numpy as np

import lrvx
from lrvx import paper

T0 = 1_788_220_800_000_000_000
STEP = 60_000_000_000
FILL_STATUSES = {"FILLED", "PARTIALLY_FILLED"}


class _Recorder(lrvx.Strategy):
    """A market buy and a far limit on the first event; cancels the limit on the third."""

    def __init__(self, symbols):
        super().__init__(symbols)
        self._seen = 0
        self._resting = 0
        self.fills = []
        self.updates = []

    def _step(self):
        self._seen += 1
        if self._seen == 1:
            self.market_buy(1.0)
            self._resting = self.limit_buy(50.0, 1.0)
        elif self._seen == 3:
            self.cancel_order(self._resting)

    def on_bar(self, ctx, bar):
        self._step()

    def on_trade(self, ctx, trade):
        self._step()

    def on_fill(self, ctx, ev):
        self.fills.append((ev.order_id, ev.status))

    def on_order_update(self, ctx, ev):
        self.updates.append((ev.order_id, ev.status))


def _backtest():
    reg = lrvx.SymbolRegistry()
    sym = reg.add_symbol("sim", "BTCUSDT", tick_size=0.5)
    strat = _Recorder([sym])
    bt = lrvx.BacktestRunner(reg, fee_rate=0.0, initial_capital=10_000)
    bt.set_strategy(strat)
    n = 5
    idx = np.arange(n)
    bt.run_bars(start_time_ns=T0 + STEP * idx, end_time_ns=T0 + STEP * (idx + 1),
                open=np.full(n, 100.0), high=np.full(n, 100.5), low=np.full(n, 99.5),
                close=np.full(n, 100.0), volume=np.full(n, 10.0), symbol="BTCUSDT")
    return strat


def _paper():
    reg = lrvx.SymbolRegistry()
    sym = int(reg.add_symbol("sim", "BTCUSDT", tick_size=0.5))
    broker = paper.PaperBroker(registry=reg)
    strat = _Recorder([sym])
    broker.runner.add_strategy(strat)
    broker.start()
    for i in range(5):
        broker.observe_trade(sym, 100.0, 1.0, i % 2 == 0, T0 + STEP * i)
    broker.stop()
    return strat


def _check(strat):
    fill_statuses = [status for _, status in strat.fills]
    update_statuses = [status for _, status in strat.updates]

    # The market buy filled, and on_fill heard of it exactly once.
    assert fill_statuses == ["FILLED"], (strat.fills, strat.updates)
    assert set(fill_statuses) <= FILL_STATUSES

    # on_order_update heard the rest of both orders' lifecycles, the cancel
    # included, and no fill.
    assert "CANCELED" in update_statuses, strat.updates
    assert not FILL_STATUSES & set(update_statuses), strat.updates
    filled_id = strat.fills[0][0]
    assert all(status not in FILL_STATUSES
               for oid, status in strat.updates if oid == filled_id)


def test_backtest_runner_sends_a_fill_to_on_fill_only():
    _check(_backtest())


def test_paper_broker_sends_a_fill_to_on_fill_only():
    _check(_paper())


def test_backtest_and_paper_route_the_same_statuses():
    bt, pp = _backtest(), _paper()
    assert {s for _, s in bt.fills} == {s for _, s in pp.fills}
    assert {s for _, s in bt.updates} == {s for _, s in pp.updates}
