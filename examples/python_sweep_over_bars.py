"""
Sweep strategy parameters over bars that each worker loads once.

The grid is parameters only. A worker process reads the bar arrays the first
time it runs a combination and keeps them; every later combination in that
process reuses them, so the cost per run is the strategy and nothing else.
Processes rather than threads: a Python strategy runs under the GIL, so two
threads would take turns rather than run at once.

Usage:
    cd /path/to/lrvx
    PYTHONPATH=build/python python3 docs/examples/python_sweep_over_bars.py
"""

import os
from concurrent.futures import ProcessPoolExecutor

import numpy as np

import lrvx

DATA = os.path.join(os.path.dirname(__file__), "data", "btcusdt_1m.csv")

_BARS = None


def bars_for(path):
    """The bar arrays run_bars takes, loaded once per process."""
    global _BARS
    if _BARS is None:
        raw = np.loadtxt(path, delimiter=",", skiprows=1)
        # The file carries millisecond timestamps; run_bars takes nanoseconds
        # and does no unit guessing.
        start_ns = raw[:, 0].astype(np.int64) * 1_000_000
        _BARS = {
            "start_time_ns": start_ns,
            "end_time_ns": start_ns + 60_000_000_000,
            "open": raw[:, 1],
            "high": raw[:, 2],
            "low": raw[:, 3],
            "close": raw[:, 4],
            "volume": raw[:, 5],
        }
    return _BARS


class SmaCross(lrvx.Strategy):
    """Long when the fast SMA is above the slow one, flat otherwise."""

    def __init__(self, symbols, fast, slow, qty=0.01):
        super().__init__(symbols)
        self.fast = lrvx.SMA(fast)
        self.slow = lrvx.SMA(slow)
        self.qty = qty

    def on_bar(self, ctx, bar):
        f = self.fast.update(bar.close)
        s = self.slow.update(bar.close)
        if f is None or s is None or not self.slow.ready:
            return
        if f > s and ctx.is_flat():
            # Held until the next bar opens: nothing fills on the bar that
            # produced the signal.
            self.market_buy(self.qty)
        elif f < s and ctx.is_long():
            # Sells exactly the open position, reduce-only.
            self.close_position()


def run_one(task):
    """One combination: a fresh registry, runner and strategy every time."""
    fast, slow, path = task
    registry = lrvx.SymbolRegistry()
    btc = registry.add_symbol("exchange", "BTCUSDT", tick_size=0.01)
    bt = lrvx.BacktestRunner(registry, fee_rate=0.0004, initial_capital=10_000.0)
    bt.set_strategy(SmaCross([btc], fast, slow))
    stats = bt.run_bars(symbol="BTCUSDT", **bars_for(path))
    return {
        "fast": fast,
        "slow": slow,
        "trades": stats["total_trades"],
        "return_pct": stats["return_pct"],
        "sharpe": stats["sharpe_ratio"],
    }


def main():
    grid = [(fast, slow, DATA) for fast in (5, 10, 20) for slow in (30, 60) if fast < slow]
    with ProcessPoolExecutor(max_workers=2) as pool:
        rows = list(pool.map(run_one, grid))
    rows.sort(key=lambda r: r["sharpe"], reverse=True)
    for r in rows:
        print(
            f"fast={r['fast']:2d} slow={r['slow']:2d}  trades={r['trades']:3d}  "
            f"return={r['return_pct']:+.4f}%  sharpe={r['sharpe']:+.3f}"
        )


if __name__ == "__main__":
    main()
