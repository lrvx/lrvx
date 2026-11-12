"""Order prices and triggers must sit on the symbol's tick in a backtest.

A venue refuses an order whose price is not a whole number of ticks, so a
backtest that accepted one would fill the strategy at a price it could never
trade at. The tests drive the Python BacktestRunner over bars, the way a
strategy computing `price * 1.005` runs.
"""

import numpy as np
import pytest

import lrvx

MINUTE_NS = 60_000_000_000


def _bars(rows):
    rows = np.asarray(rows, dtype=float)
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


# Bar 0 opens the position (a buy held to bar 1's open, 100.3). Bar 1 arms
# the exit at its open. Later bars trade far enough up to reach any target.
BARS = [
    (100.0, 100.0, 100.0, 100.0),
    (100.3, 100.4, 100.2, 100.3),
    (100.1, 101.5, 100.0, 101.0),
    (101.0, 102.0, 100.5, 101.5),
]


class EntryThenExit(lrvx.Strategy):
    """Buys on the first bar, then places one exit priced from the fill."""

    def __init__(self, symbols, place_exit):
        super().__init__(symbols)
        self.place_exit = place_exit
        self.events = []
        self.entry = None
        self.bar = 0

    def on_bar(self, ctx, bar):
        if self.bar == 0:
            self.market_buy(1.0)
        elif self.bar == 1 and self.entry is not None:
            self.place_exit(self, self.entry)
        self.bar += 1

    def on_order_update(self, ctx, ev):
        self.events.append((ev.order_type, ev.status, ev.fill_price, ev.reject_reason))

    def on_fill(self, ctx, ev):
        self.events.append((ev.order_type, "FILLED", ev.fill_price, ev.reject_reason))
        if self.entry is None:
            self.entry = ev.fill_price


def _run(place_exit, **symbol_kwargs):
    registry = lrvx.SymbolRegistry()
    sym = registry.add_symbol("bybit", "BTCUSDT", **symbol_kwargs)
    bt = lrvx.BacktestRunner(registry, fee_rate=0.0, initial_capital=100_000.0)
    strat = EntryThenExit([sym], place_exit)
    bt.set_strategy(strat)
    bt.run_bars(**_bars(BARS))
    return strat.events


def _exit_events(events):
    return [e for e in events if e[0] != "market"]


def test_take_profit_priced_from_the_fill_is_rejected_off_tick():
    events = _run(lambda s, entry: s.take_profit_market("sell", entry * 1.005, 1.0),
                  tick_size=0.1)
    exits = _exit_events(events)
    assert exits, events
    assert exits[0][1] == "REJECTED"
    assert exits[0][3] == "trigger_off_tick"
    assert not [e for e in exits if e[1] == "FILLED"]


def test_take_profit_rounded_by_the_strategy_fills_on_the_tick():
    def place(s, entry):
        target = round(entry * 1.005 / 0.1) * 0.1
        s.take_profit_market("sell", target, 1.0)

    exits = _exit_events(_run(place, tick_size=0.1))
    fills = [e for e in exits if e[1] == "FILLED"]
    assert len(fills) == 1, exits
    assert fills[0][2] == pytest.approx(100.8, abs=1e-9)


def test_off_tick_limit_is_rejected_with_its_reason():
    exits = _exit_events(_run(lambda s, entry: s.limit_sell(entry * 1.0037, 1.0),
                              tick_size=0.1))
    assert exits[0][1] == "REJECTED"
    assert exits[0][3] == "price_off_tick"


def test_off_tick_stop_and_trailing_offset_are_rejected():
    stop = _exit_events(_run(lambda s, entry: s.stop_market("sell", entry * 0.9963, 1.0),
                             tick_size=0.1))
    assert (stop[0][1], stop[0][3]) == ("REJECTED", "trigger_off_tick")

    trail = _exit_events(_run(lambda s, entry: s.trailing_stop("sell", 0.25, 1.0),
                              tick_size=0.1))
    assert (trail[0][1], trail[0][3]) == ("REJECTED", "trailing_offset_off_tick")


def test_symbol_registered_without_a_tick_is_not_checked():
    # The registry falls back to 0.01 for its book, but that is not the
    # venue's tick, so nothing is refused against it.
    exits = _exit_events(_run(lambda s, entry: s.take_profit_market("sell", entry * 1.005, 1.0)))
    assert not [e for e in exits if e[1] == "REJECTED"], exits
    assert [e for e in exits if e[1] == "FILLED"]


def test_explicit_cent_tick_is_checked():
    exits = _exit_events(_run(lambda s, entry: s.limit_sell(entry + 0.005, 1.0),
                              tick_size=0.01))
    assert (exits[0][1], exits[0][3]) == ("REJECTED", "price_off_tick")
