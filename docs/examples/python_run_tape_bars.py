"""A bar strategy over a .lrvx tape: the one-call route and the explicit one.

run_tape_bars aggregates the tape into time bars while it streams and replays
them with run_bars' fill rules. The explicit route -- read_trades,
aggregate_time_bars, run_bars -- lands on the same fills, trades and equity
curve for the same interval.
"""
import tempfile

import numpy as np

import lrvx

INTERVAL = 300.0
SEC = 10**9


def write_tape(path: str) -> None:
    """Six hours of synthetic BTCUSDT trades, symbol id 1."""
    rng = np.random.default_rng(1)
    n = 20_000
    ts = 1_767_225_600 * SEC + np.cumsum(rng.integers(1, 2 * SEC, n))
    px = np.round(60_000 + np.cumsum(rng.integers(-50, 51, n)) * 0.1, 1)
    qty = np.round(rng.uniform(0.001, 0.5, n), 3)
    w = lrvx.DataWriter(path)
    w.write_trades(ts, ts, px, qty, np.arange(n, dtype=np.uint64),
                   np.ones(n, np.uint32), rng.integers(0, 2, n).astype(np.uint8))
    w.close()


class Breakout(lrvx.Strategy):
    """Buys a close above the previous high, exits a close below the previous low."""

    def __init__(self, symbols):
        super().__init__(symbols)
        self.prev = None

    def on_bar(self, ctx, bar):
        if self.prev is not None:
            if ctx.is_flat() and bar.close > self.prev.high:
                self.market_buy(0.1)
            elif not ctx.is_flat() and bar.close < self.prev.low:
                self.close_position()
        self.prev = bar


def runner():
    reg = lrvx.SymbolRegistry()
    btc = reg.add_symbol("bybit", "BTCUSDT", tick_size=0.1)
    bt = lrvx.BacktestRunner(reg, fee_rate=0.00055, initial_capital=10_000.0,
                             slippage_model="fixed_bps", slippage_bps=1.0)
    bt.set_strategy(Breakout([btc]))
    return bt


def one_call(tape: str):
    bt = runner()
    stats = bt.run_tape_bars(tape, interval_seconds=INTERVAL, symbol="BTCUSDT")
    return stats, bt.trades(), bt.equity_curve()


def explicit(tape: str):
    trades = lrvx.DataReader(tape).read_trades()
    bars = lrvx.aggregate_time_bars(
        trades["exchange_ts_ns"], trades["price_raw"] / lrvx.PRICE_SCALE,
        trades["qty_raw"] / lrvx.VOLUME_SCALE, (trades["side"] == 0).astype(np.uint8),
        interval_seconds=INTERVAL)
    bt = runner()
    stats = bt.run_bars(
        start_time_ns=bars["start_time_ns"], end_time_ns=bars["end_time_ns"],
        open=bars["open_raw"] / lrvx.PRICE_SCALE, high=bars["high_raw"] / lrvx.PRICE_SCALE,
        low=bars["low_raw"] / lrvx.PRICE_SCALE, close=bars["close_raw"] / lrvx.PRICE_SCALE,
        volume=bars["volume_raw"] / lrvx.VOLUME_SCALE, symbol="BTCUSDT",
        bar_type=0, bar_type_param=int(INTERVAL * SEC))
    return stats, bt.trades(), bt.equity_curve()


def main() -> None:
    with tempfile.TemporaryDirectory() as tape:
        write_tape(tape)
        a = one_call(tape)
        b = explicit(tape)
    assert a[0] == b[0], "stats differ"
    for got, want in ((a[1], b[1]), (a[2], b[2])):
        for key in got:
            assert np.array_equal(got[key], want[key]), key
    stats = a[0]
    print(f"trades {stats['total_trades']}  net pnl {stats['net_pnl']:.2f}  "
          f"max dd {stats['max_drawdown_pct']:.2f}%  (both routes identical)")


if __name__ == "__main__":
    main()
