"""
A bar strategy with a Chandelier exit, next to the same entry without one.

The entry is the reference backtest's SMA(10) over SMA(40) cross on the
one-minute file that ships with the docs. The exit rule hangs a stop three
ATRs below the highest high since the entry and only ever raises it; the
strategy arms it on the bar the position opens (the entry is that bar's
open, where a callback order fills), updates it with every bar, and closes
the position on the bar whose range reaches the stop.

Usage:
    cd /path/to/lrvx
    PYTHONPATH=build/python python3 docs/examples/python_bar_exits.py
"""

import os

import numpy as np

import lrvx
from lrvx.exits import ChandelierExit

DATA = os.path.join(os.path.dirname(__file__), "data", "btcusdt_1m.csv")


class SmaCross(lrvx.Strategy):
    """Long while SMA(10) is above SMA(40); flat on the cross down, or on the
    exit rule when one is given."""

    def __init__(self, symbols, exit_rule=None, atr_period=14, qty=0.01):
        super().__init__(symbols)
        self.fast = lrvx.SMA(10)
        self.slow = lrvx.SMA(40)
        self.atr = lrvx.ATR(atr_period)
        self.exit_rule = exit_rule
        self.qty = qty
        self.stopped_out = 0

    def on_bar(self, ctx, bar):
        f = self.fast.update(bar.close)
        s = self.slow.update(bar.close)
        atr = self.atr.update(bar.high, bar.low, bar.close)
        if f is None or s is None or atr is None:
            return

        rule = self.exit_rule
        if rule is not None and ctx.is_long():
            if not rule.armed:
                # The buy was held through the signal bar and filled at this
                # bar's open; the stop has been resting since then.
                rule.arm("long", bar.open, atr)
            if rule.update(bar.high, bar.low, atr):
                self.close_position()
                rule.disarm()
                self.stopped_out += 1
                return

        if f > s and ctx.is_flat():
            self.market_buy(self.qty)
        elif f < s and ctx.is_long():
            self.close_position()
            if rule is not None:
                rule.disarm()


def run(exit_rule):
    raw = np.loadtxt(DATA, delimiter=",", skiprows=1)
    start_ns = raw[:, 0].astype(np.int64) * 1_000_000
    registry = lrvx.SymbolRegistry()
    btc = registry.add_symbol("exchange", "BTCUSDT", tick_size=0.01)
    bt = lrvx.BacktestRunner(registry, fee_rate=0.0004, initial_capital=10_000.0)
    strat = SmaCross([btc], exit_rule)
    bt.set_strategy(strat)
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
    return stats, strat.stopped_out


def main():
    plain, _ = run(None)
    guarded, stopped = run(ChandelierExit(lookback=0, atr_multiple=3.0))
    print(f"{'':18s}{'trades':>8s}{'stopped':>9s}{'net pnl':>10s}{'max dd %':>10s}")
    print(f"{'cross only':18s}{plain['total_trades']:8d}{0:9d}"
          f"{plain['net_pnl']:10.2f}{plain['max_drawdown_pct']:10.3f}")
    print(f"{'with chandelier':18s}{guarded['total_trades']:8d}{stopped:9d}"
          f"{guarded['net_pnl']:10.2f}{guarded['max_drawdown_pct']:10.3f}")


if __name__ == "__main__":
    main()
