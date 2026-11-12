"""A runner built with slippage or latency settings still checks the tick.

The tick rule and the runner's fill settings reach the executor through
different construction paths; an off-tick price must be rejected on both.
"""
from __future__ import annotations

import numpy as np
import pytest

import lrvx

T0 = 1_788_220_800_000_000_000
STEP = 60_000_000_000
N = 5


class OffTickOnce(lrvx.Strategy):
    def __init__(self, symbols, events):
        super().__init__(symbols)
        self._sent = False
        self._events = events

    def on_bar(self, ctx, bar):
        if not self._sent:
            self._sent = True
            self.limit_buy(bar.close - 0.0333, 0.01)

    def on_order_update(self, ctx, ev):
        self._events.append((ev.status, ev.reject_reason))


def _bars():
    full = lambda v: np.full(N, v, dtype=float)  # noqa: E731
    idx = np.arange(N)
    return dict(start_time_ns=T0 + STEP * idx, end_time_ns=T0 + STEP * (idx + 1),
                open=full(100.0), high=full(101.0), low=full(99.0), close=full(100.0),
                volume=full(1.0), symbol="BTCUSDT")


@pytest.mark.parametrize("settings", [
    {},
    {"slippage_model": "fixed_bps", "slippage_bps": 1.0},
    {"submit_ack_latency_ns": 5_000_000},
])
def test_off_tick_price_is_rejected_whatever_the_fill_settings(settings):
    reg = lrvx.SymbolRegistry()
    sym = reg.add_symbol("bybit", "BTCUSDT", tick_size=0.1)
    events = []
    bt = lrvx.BacktestRunner(reg, fee_rate=0.0004, initial_capital=10_000, **settings)
    bt.set_strategy(OffTickOnce([sym], events))
    bt.run_bars(**_bars())
    assert ("REJECTED", "price_off_tick") in events, events
