"""ctx.position inside on_fill already counts the fill being delivered.

The runner's position tracker takes a fill before the strategy hears of it,
so ``ctx.position`` and ``position()`` agree inside ``on_fill`` and
``on_order_update``, in a backtest and in paper mode.
"""
from __future__ import annotations

import numpy as np
import pytest

import lrvx
from lrvx import paper

NS = 1_000_000_000
PRICES = [100.0, 100.5, 101.0, 100.5, 100.0, 99.5, 100.0, 100.5]


class InAndOut(lrvx.Strategy):
    """Buys 1, sells 0.4, then closes; a parked limit is cancelled at the end."""

    def __init__(self, symbols):
        super().__init__(symbols)
        self.seen = 0
        self.parked = 0
        self.fills = []    # (side, fill_qty, ctx.position, position())
        self.updates = []  # (status, ctx.position, position())

    def on_trade(self, ctx, trade):
        self.seen += 1
        if self.seen == 1:
            self.market_buy(1.0)
            self.parked = self.limit_buy(50.0, 1.0)
        elif self.seen == 3:
            self.market_sell(0.4)
        elif self.seen == 5:
            self.close_position()
        elif self.seen == 7:
            self.cancel_order(self.parked)

    def on_fill(self, ctx, ev):
        self.fills.append((ev.side, ev.fill_qty, ctx.position, self.position()))

    def on_order_update(self, ctx, ev):
        self.updates.append((ev.status, ctx.position, self.position()))


def _registry():
    registry = lrvx.SymbolRegistry()
    sym = registry.add_symbol("sim", "BTCUSDT", tick_size=0.5)
    return registry, sym


def _backtest():
    registry, sym = _registry()
    strat = InAndOut([sym])
    runner = lrvx.BacktestRunner(registry, 0.0, 100_000.0)
    runner.set_strategy(strat)
    ts = np.array([(i + 1) * NS for i in range(len(PRICES))], dtype=np.int64)
    runner.run_ohlcv(ts, np.array(PRICES), "BTCUSDT")
    return strat


def _paper():
    registry, sym = _registry()
    broker = paper.PaperBroker(registry=registry)
    strat = InAndOut([sym])
    broker.runner.add_strategy(strat)
    broker.start()
    for i, px in enumerate(PRICES):
        broker.observe_trade(int(sym), px, 1.0, True, (i + 1) * NS)
    broker.stop()
    return strat


@pytest.mark.parametrize("run", [_backtest, _paper], ids=["backtest", "paper"])
def test_ctx_position_in_on_fill_includes_the_fill(run):
    strat = run()

    assert [(side, qty) for side, qty, _, _ in strat.fills] == [
        ("buy", 1.0), ("sell", 0.4), ("sell", 0.6)]
    after = [1.0, 0.6, 0.0]
    for (side, qty, ctx_position, position), want in zip(strat.fills, after):
        assert position == pytest.approx(want)
        assert ctx_position == pytest.approx(want), (
            f"ctx.position inside on_fill for the {side} of {qty} should be the position "
            f"after it")


@pytest.mark.parametrize("run", [_backtest, _paper], ids=["backtest", "paper"])
def test_ctx_position_in_on_order_update_is_current(run):
    strat = run()

    assert strat.updates, "the parked limit produced order updates"
    for status, ctx_position, position in strat.updates:
        assert ctx_position == pytest.approx(position), status
    canceled = [u for u in strat.updates if u[0] == "CANCELED"]
    assert len(canceled) == 1
    assert canceled[0][1] == pytest.approx(0.0)


def test_backtest_and_paper_agree():
    bt, pp = _backtest(), _paper()
    assert pp.fills == bt.fills
    assert pp.updates == bt.updates
