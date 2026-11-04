"""
The reference backtest: one strategy, one data file, pinned numbers.

Everything here is fixed so the output is a number to check an engine
against, not a result to interpret: SMA(10) over SMA(40) on the close of
one-minute bars, long 0.01 when the fast average is above the slow one, flat
otherwise, taker fee 0.0004 on every fill, initial capital 10 000. The
expected values below are the ones the engine produces today; the script
fails if they move.

Usage:
    cd /path/to/lrvx
    PYTHONPATH=build/python python3 docs/examples/python_reference_backtest.py
"""

import os

import numpy as np

import lrvx

DATA = os.path.join(os.path.dirname(__file__), "data", "btcusdt_1m.csv")

EXPECTED = {
    "total_trades": 47,
    "winning_trades": 16,
    "losing_trades": 31,
    "gross_profit": 28.0608,
    "gross_loss": 42.7754,
    "total_pnl": -14.7146,
    "total_fees": 28.37314883,
    "net_pnl": -43.08774883,
    "final_capital": 9956.91225117,
    "return_pct": -0.4308774883,
}

# The first and the last closed trade. entry_time_ns is the end of the bar the
# entry filled in, which is the bar after the one whose close produced the
# signal; the entry price is that bar's open.
FIRST_TRADE = {"entry_time_ns": 1776609420000000000, "entry_price": 75822.85,
               "exit_time_ns": 1776611280000000000, "exit_price": 75909.76,
               "pnl": 0.8691, "fee": 0.60693044}
LAST_TRADE = {"entry_time_ns": 1776781140000000000, "entry_price": 76185.83,
              "exit_time_ns": 1776783420000000000, "exit_price": 76005.21,
              "pnl": -1.8062, "fee": 0.60876416}


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


def load_bars(path):
    raw = np.loadtxt(path, delimiter=",", skiprows=1)
    start_ns = raw[:, 0].astype(np.int64) * 1_000_000
    return {
        "start_time_ns": start_ns,
        "end_time_ns": start_ns + 60_000_000_000,
        "open": raw[:, 1],
        "high": raw[:, 2],
        "low": raw[:, 3],
        "close": raw[:, 4],
        "volume": raw[:, 5],
    }


def run():
    bars = load_bars(DATA)
    registry = lrvx.SymbolRegistry()
    btc = registry.add_symbol("exchange", "BTCUSDT", tick_size=0.01)
    bt = lrvx.BacktestRunner(registry, fee_rate=0.0004, initial_capital=10_000.0)
    bt.set_strategy(SmaCross([btc]))
    stats = bt.run_bars(symbol="BTCUSDT", **bars)
    return bars, stats, bt.trades()


def main():
    bars, stats, trades = run()
    mismatches = []
    for key, want in EXPECTED.items():
        got = stats[key]
        if abs(got - want) > 1e-9:
            mismatches.append(f"{key}: expected {want}, got {got}")
    first = {k: trades[k][0].item() for k in FIRST_TRADE}
    last = {k: trades[k][-1].item() for k in LAST_TRADE}
    for name, got, want in (("first trade", first, FIRST_TRADE), ("last trade", last, LAST_TRADE)):
        for k in want:
            if abs(float(got[k]) - float(want[k])) > 1e-9:
                mismatches.append(f"{name} {k}: expected {want[k]}, got {got[k]}")
    # The entry fills at the open of the bar after the signal bar: the slow
    # average is ready on the 40th bar, so the first buy fills on the 41st.
    if abs(float(bars["open"][40]) - FIRST_TRADE["entry_price"]) > 1e-9:
        mismatches.append("first entry price is not the open of bar 40")
    if int(bars["end_time_ns"][40]) != FIRST_TRADE["entry_time_ns"]:
        mismatches.append("first entry time is not the end of bar 40")

    print(f"bars: {len(bars['close'])}")
    for key in EXPECTED:
        print(f"  {key:16s} {stats[key]}")
    print(f"  first trade      {first}")
    print(f"  last trade       {last}")
    if mismatches:
        raise SystemExit("reference backtest moved:\n  " + "\n  ".join(mismatches))
    print("reference backtest matches")


if __name__ == "__main__":
    main()
