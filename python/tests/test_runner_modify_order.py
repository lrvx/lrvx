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


class StopThenModify(lrvx.Strategy):
    """A sell stop on the first bar, modified on the third."""

    def __init__(self, symbols, trigger, new_trigger, new_qty):
        super().__init__(symbols)
        self._trigger = trigger
        self._new_trigger = new_trigger
        self._new_qty = new_qty
        self._seen = 0
        self.order_id = 0
        self.fills = []
        self.events = []

    def on_bar(self, ctx, bar):
        self._seen += 1
        if self._seen == 1:
            self.order_id = self.stop_market("sell", self._trigger, 1.0)
        elif self._seen == 3 and self._new_trigger is not None:
            self.modify_order(self.order_id, self._new_trigger, self._new_qty)

    def on_fill(self, ctx, ev):
        self.fills.append(ev)

    def on_order_update(self, ctx, ev):
        self.events.append(ev)


def _run_stop(new_trigger, new_qty=2.0):
    reg = lrvx.SymbolRegistry()
    sym = reg.add_symbol("test", "BTCUSDT", tick_size=0.5)
    strat = StopThenModify([sym], 95.0, new_trigger, new_qty)
    bt = lrvx.BacktestRunner(reg, fee_rate=0.0, initial_capital=10_000)
    bt.set_strategy(strat)
    bt.run_bars(**_bars(100.5, 97.0))
    return strat


def test_modify_moves_the_trigger_of_an_untriggered_stop():
    assert not _run_stop(None).fills, "a stop at 95 is never reached by a low of 97"

    strat = _run_stop(98.0)
    statuses = [e.status for e in strat.events]
    assert statuses.count("REPLACED") == 1, statuses
    assert "TRIGGERED" in statuses, statuses
    assert len(strat.fills) == 1, statuses
    fill = strat.fills[0]
    assert fill.order_id == strat.order_id
    assert fill.side == "sell"
    assert fill.order_type == "stop_market"
    assert fill.fill_qty == 2.0


def test_off_tick_trigger_modify_is_rejected():
    strat = _run_stop(98.25)
    rejects = [e for e in strat.events if e.status == "REPLACE_REJECTED"]
    assert len(rejects) == 1, [e.status for e in strat.events]
    assert rejects[0].reject_reason == "trigger_off_tick"
    assert not strat.fills


class ModifyAfterFill(lrvx.Strategy):
    def __init__(self, symbols):
        super().__init__(symbols)
        self._seen = 0
        self.order_id = 0
        self.fills = []
        self.events = []

    def on_bar(self, ctx, bar):
        self._seen += 1
        if self._seen == 1:
            self.order_id = self.market_buy(1.0)
        elif self._seen == 3:
            self.modify_order(self.order_id, 99.0, 2.0)
            self.modify_order(987654, 99.0, 2.0)

    def on_fill(self, ctx, ev):
        self.fills.append(ev)

    def on_order_update(self, ctx, ev):
        self.events.append(ev)


def test_modify_of_a_finished_order_is_rejected():
    reg = lrvx.SymbolRegistry()
    sym = reg.add_symbol("test", "BTCUSDT", tick_size=0.5)
    strat = ModifyAfterFill([sym])
    bt = lrvx.BacktestRunner(reg, fee_rate=0.0, initial_capital=10_000)
    bt.set_strategy(strat)
    bt.run_bars(**_bars(100.5, 99.5))

    assert len(strat.fills) == 1
    rejects = [e for e in strat.events if e.status == "REPLACE_REJECTED"]
    # The filled order's refusal comes back on its symbol. The id nobody ever
    # sent belongs to no symbol, so no strategy hears of it.
    assert [(e.order_id, e.reject_reason) for e in rejects] == [(strat.order_id, "unknown_order")]
    assert rejects[0].symbol_id == int(sym)
