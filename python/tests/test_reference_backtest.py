"""The reference backtest produces the pinned numbers.

docs/explanation/reference-backtest.md fixes one strategy, one data file and
one configuration, and lists what the engine produces for them. This test is
that list: a change that moves any of these values is a change to the
execution model (fill timing, fill price, fees, trade accounting) and has to
update the page with it.
"""

from __future__ import annotations

import os

import numpy as np

import lrvx

CSV = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "..", "docs", "examples", "data", "btcusdt_1m.csv")
)


class SmaCross(lrvx.Strategy):
    def __init__(self, symbols, fast=10, slow=40, qty=0.01):
        super().__init__(symbols)
        self.fast = lrvx.SMA(fast)
        self.slow = lrvx.SMA(slow)
        self.qty = qty

    def on_bar(self, ctx, bar):
        f = self.fast.update(bar.close)
        s = self.slow.update(bar.close)
        if f is None or s is None:
            return
        if f > s and ctx.is_flat():
            self.market_buy(self.qty)
        elif f < s and ctx.is_long():
            self.close_position()


def _run():
    raw = np.loadtxt(CSV, delimiter=",", skiprows=1)
    start_ns = raw[:, 0].astype(np.int64) * 1_000_000
    registry = lrvx.SymbolRegistry()
    btc = registry.add_symbol("exchange", "BTCUSDT", tick_size=0.01)
    bt = lrvx.BacktestRunner(registry, fee_rate=0.0004, initial_capital=10_000.0)
    bt.set_strategy(SmaCross([btc]))
    stats = bt.run_bars(
        start_time_ns=start_ns,
        end_time_ns=start_ns + 60_000_000_000,
        open=raw[:, 1],
        high=raw[:, 2],
        low=raw[:, 3],
        close=raw[:, 4],
        volume=raw[:, 5],
        symbol="BTCUSDT",
    )
    return raw, stats, bt.trades()


def test_reference_backtest_statistics_are_pinned() -> None:
    _, stats, _ = _run()
    assert stats["total_trades"] == 47
    assert stats["winning_trades"] == 16
    assert stats["losing_trades"] == 31
    assert abs(stats["gross_profit"] - 28.0608) < 1e-9
    assert abs(stats["gross_loss"] - 42.7754) < 1e-9
    assert abs(stats["total_pnl"] - (-14.7146)) < 1e-9
    assert abs(stats["total_fees"] - 28.37314883) < 1e-9
    assert abs(stats["net_pnl"] - (-43.08774883)) < 1e-9
    assert abs(stats["final_capital"] - 9956.91225117) < 1e-9
    assert abs(stats["return_pct"] - (-0.4308774883)) < 1e-9


def test_reference_backtest_first_and_last_trade_are_pinned() -> None:
    raw, _, trades = _run()
    assert int(trades["entry_time_ns"][0]) == 1776609420000000000
    assert abs(float(trades["entry_price"][0]) - 75822.85) < 1e-9
    assert int(trades["exit_time_ns"][0]) == 1776611280000000000
    assert abs(float(trades["exit_price"][0]) - 75909.76) < 1e-9
    assert abs(float(trades["pnl"][0]) - 0.8691) < 1e-9
    assert abs(float(trades["fee"][0]) - 0.60693044) < 1e-9

    assert int(trades["entry_time_ns"][-1]) == 1776781140000000000
    assert abs(float(trades["entry_price"][-1]) - 76185.83) < 1e-9
    assert int(trades["exit_time_ns"][-1]) == 1776783420000000000
    assert abs(float(trades["exit_price"][-1]) - 76005.21) < 1e-9
    assert abs(float(trades["pnl"][-1]) - (-1.8062)) < 1e-9
    assert abs(float(trades["fee"][-1]) - 0.60876416) < 1e-9


def test_reference_entry_fills_at_the_open_of_the_bar_after_the_signal() -> None:
    raw, _, trades = _run()
    # SMA(40) is ready on the 40th close (index 39); the buy it produces fills
    # at the open of bar 40 and is stamped with that bar's end time.
    assert abs(float(trades["entry_price"][0]) - float(raw[40, 1])) < 1e-9
    assert int(trades["entry_time_ns"][0]) == int(raw[40, 0]) * 1_000_000 + 60_000_000_000
