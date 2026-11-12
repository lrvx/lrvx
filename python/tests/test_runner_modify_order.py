"""modify_order in a backtest keeps the order it modifies.

The replacement is the same order with a new price and quantity: it stays on
its symbol and side, answers to the id the strategy holds, and fills at the
new price.
"""
from __future__ import annotations

import numpy as np
import pytest

import lrvx

T0 = 1_788_220_800_000_000_000
STEP = 60_000_000_000
N = 6


class ModifyOnce(lrvx.Strategy):
    """Limit on the first bar, modify on the third, optional cancel on the fourth."""

    def __init__(self, symbols, side, rest_px, new_px, new_qty, cancel_after=False):
        super().__init__(symbols)
        self._side = side
        self._rest_px = rest_px
        self._new_px = new_px
        self._new_qty = new_qty
        self._cancel_after = cancel_after
        self._seen = 0
        self.order_id = 0
        self.fills = []
        self.events = []

    def on_bar(self, ctx, bar):
        self._seen += 1
        if self._seen == 1:
            send = self.limit_buy if self._side == "buy" else self.limit_sell
            self.order_id = send(self._rest_px, 1.0)
        elif self._seen == 3:
            self.modify_order(self.order_id, self._new_px, self._new_qty)
        elif self._seen == 4 and self._cancel_after:
            self.cancel_order(self.order_id)

    def on_fill(self, ctx, ev):
        self.fills.append(ev)

    def on_order_update(self, ctx, ev):
        self.events.append(ev)


def _bars(spike_high, spike_low):
    """Six bars around 100; only the fifth leaves the 99.5 .. 100.5 band."""
    idx = np.arange(N)
    high = np.full(N, 100.5)
    low = np.full(N, 99.5)
    high[4] = spike_high
    low[4] = spike_low
    return dict(start_time_ns=T0 + STEP * idx, end_time_ns=T0 + STEP * (idx + 1),
                open=np.full(N, 100.0), high=high, low=low, close=np.full(N, 100.0),
                volume=np.full(N, 10.0), symbol="BTCUSDT")


def _run(strategy_args, bars):
    reg = lrvx.SymbolRegistry()
    sym = reg.add_symbol("bybit", "BTCUSDT", tick_size=0.5)
    strat = ModifyOnce([sym], *strategy_args)
    bt = lrvx.BacktestRunner(reg, fee_rate=0.0, initial_capital=10_000)
    bt.set_strategy(strat)
    bt.run_bars(**bars)
    return int(sym), strat


@pytest.mark.parametrize("side,rest_px,new_px,new_qty,bars", [
    ("buy", 90.0, 99.0, 2.0, _bars(100.5, 98.0)),
    ("sell", 110.0, 101.0, 0.5, _bars(102.0, 99.5)),
])
def test_modified_order_fills_at_the_new_price(side, rest_px, new_px, new_qty, bars):
    sym, strat = _run((side, rest_px, new_px, new_qty), bars)

    assert strat.order_id != 0
    assert len(strat.fills) == 1, [e.status for e in strat.events]
    fill = strat.fills[0]
    assert fill.status == "FILLED"
    assert fill.order_id == strat.order_id
    assert fill.symbol_id == sym
    assert fill.side == side
    assert fill.fill_price == new_px
    assert fill.fill_qty == new_qty


def test_modified_order_cancels_by_the_same_id():
    sym, strat = _run(("buy", 90.0, 99.0, 2.0, True), _bars(100.5, 98.0))

    canceled = [e for e in strat.events if e.status == "CANCELED"]
    assert len(canceled) == 1, [e.status for e in strat.events]
    assert canceled[0].order_id == strat.order_id
    assert canceled[0].symbol_id == sym
    assert not strat.fills


def test_off_tick_modify_is_rejected_and_the_order_is_left_alone():
    _, strat = _run(("buy", 90.0, 99.25, 2.0), _bars(100.5, 98.0))

    rejects = [e for e in strat.events if e.status == "REPLACE_REJECTED"]
    assert len(rejects) == 1, [e.status for e in strat.events]
    assert rejects[0].reject_reason == "price_off_tick"
    assert not strat.fills
