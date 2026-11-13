"""Order quantities must sit on the symbol's lot in a backtest.

A venue refuses an order whose quantity is not a whole number of its quantity
step, so a backtest that accepted one would trade a size the venue never
would. A symbol declares a lot only when add_symbol is given one.
"""

import numpy as np
import pytest

import lrvx

MINUTE_NS = 60_000_000_000


def _bars():
    rows = np.asarray([(100.0, 100.5, 99.5, 100.0)] * 3, dtype=float)
    start = 1_700_000_000_000_000_000 + np.arange(len(rows), dtype=np.int64) * MINUTE_NS
    return dict(
        start_time_ns=start,
        end_time_ns=start + MINUTE_NS,
        open=rows[:, 0],
        high=rows[:, 1],
        low=rows[:, 2],
        close=rows[:, 3],
        volume=np.full(len(rows), 1000.0),
        symbol="BTCUSDT",
    )


class OneOrder(lrvx.Strategy):
    """Sends one order on the first bar, optionally modifies it on the second."""

    def __init__(self, symbols, place, modify_qty=None):
        super().__init__(symbols)
        self.place = place
        self.modify_qty = modify_qty
        self.events = []
        self.order_id = None
        self.bar = 0

    def on_bar(self, ctx, bar):
        if self.bar == 0:
            self.order_id = self.place(self)
        elif self.bar == 1 and self.modify_qty is not None:
            self.modify_order(self.order_id, 99.0, self.modify_qty)
        self.bar += 1

    def on_order_update(self, ctx, ev):
        self.events.append((ev.status, ev.reject_reason))

    def on_fill(self, ctx, ev):
        self.events.append(("FILLED", ev.reject_reason))


def _run(place, modify_qty=None, **symbol_kwargs):
    registry = lrvx.SymbolRegistry()
    sym = registry.add_symbol("bybit", "BTCUSDT", **symbol_kwargs)
    bt = lrvx.BacktestRunner(registry, fee_rate=0.0, initial_capital=100_000.0)
    strat = OneOrder([sym], place, modify_qty)
    bt.set_strategy(strat)
    bt.run_bars(**_bars())
    return strat.events


def test_off_lot_limit_is_rejected_with_its_reason():
    events = _run(lambda s: s.limit_buy(99.0, 0.0015), tick_size=0.1, lot_size=0.001)
    assert events[0] == ("REJECTED", "quantity_off_lot"), events


def test_off_lot_market_order_is_rejected():
    events = _run(lambda s: s.market_buy(1.0005), tick_size=0.1, lot_size=0.001)
    assert ("REJECTED", "quantity_off_lot") in events, events
    assert not [e for e in events if e[0] == "FILLED"]


def test_on_lot_order_is_accepted():
    events = _run(lambda s: s.limit_buy(99.0, 0.002), tick_size=0.1, lot_size=0.001)
    assert not [e for e in events if e[0] == "REJECTED"], events


def test_symbol_registered_without_a_lot_is_not_checked():
    events = _run(lambda s: s.limit_buy(99.0, 0.00012345), tick_size=0.1)
    assert not [e for e in events if e[0] == "REJECTED"], events


def test_a_lot_alone_checks_quantities_and_not_prices():
    events = _run(lambda s: s.limit_buy(99.005, 0.002), lot_size=0.001)
    assert not [e for e in events if e[0] == "REJECTED"], events
    events = _run(lambda s: s.limit_buy(99.005, 0.0025), lot_size=0.001)
    assert events[0] == ("REJECTED", "quantity_off_lot"), events


def test_off_lot_modify_is_refused_and_says_why():
    events = _run(lambda s: s.limit_buy(99.0, 0.002), modify_qty=0.0025,
                  tick_size=0.1, lot_size=0.001)
    assert ("REPLACE_REJECTED", "quantity_off_lot") in events, events


def test_symbol_carries_the_lot_it_was_registered_with():
    registry = lrvx.SymbolRegistry()
    with_lot = registry.add_symbol("bybit", "BTCUSDT", tick_size=0.1, lot_size=0.001)
    without = registry.add_symbol("bybit", "ETHUSDT", tick_size=0.01)
    assert with_lot.lot_size == pytest.approx(0.001)
    assert without.lot_size is None


def test_a_lot_that_is_not_positive_is_refused():
    registry = lrvx.SymbolRegistry()
    with pytest.raises(ValueError):
        registry.add_symbol("bybit", "BTCUSDT", lot_size=0.0)
    with pytest.raises(ValueError):
        registry.add_symbol("bybit", "BTCUSDT", lot_size=-1.0)
    assert registry.symbol_count() == 0
