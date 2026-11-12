"""run_tape / run_tapes count the tape records they read but cannot deliver.

Option-quote and pool-state records share the tape's frame stream with trades
and books. No Strategy callback takes either, and the simulated executor does
not move on them, so the runner skips them -- visibly: the stats of a tape run
carry skipped_option_quotes and skipped_pool_states.
"""
from __future__ import annotations

import numpy as np

import lrvx

T0 = 1_700_000_000_000_000_000


def _tape(path, n_trades: int, n_quotes: int) -> None:
    w = lrvx.DataWriter(str(path), max_segment_mb=16, exchange_id=0)
    ts = T0 + np.arange(n_trades, dtype=np.int64) * 1_000_000_000
    w.write_trades(
        exchange_ts_ns=ts, recv_ts_ns=ts, prices=np.full(n_trades, 100.0),
        quantities=np.full(n_trades, 0.1), trade_ids=np.arange(1, n_trades + 1, dtype=np.uint64),
        symbol_ids=np.full(n_trades, 1, dtype=np.uint32), sides=np.zeros(n_trades, dtype=np.uint8))
    if n_quotes:
        qts = T0 + 500_000_000 + np.arange(n_quotes, dtype=np.int64) * 1_000_000_000
        w.write_option_quotes(
            exchange_ts_ns=qts, recv_ts_ns=qts, mark_prices=np.full(n_quotes, 0.05),
            index_prices=np.full(n_quotes, 100.0), ivs=np.full(n_quotes, 0.6),
            open_interest=np.full(n_quotes, 10.0),
            symbol_ids=np.full(n_quotes, 2, dtype=np.uint32))
    w.close()


class _Count(lrvx.Strategy):
    def __init__(self, symbols):
        super().__init__(symbols)
        self.trades = 0

    def on_trade(self, ctx, trade):
        self.trades += 1


def _runner():
    reg = lrvx.SymbolRegistry()
    sym = reg.add_symbol("test", "AAA", tick_size=0.01)
    bt = lrvx.BacktestRunner(reg, 0.0, 10_000.0)
    strat = _Count([sym.id])
    bt.set_strategy(strat)
    return bt, strat


def test_run_tape_reports_skipped_option_quotes(tmp_path):
    _tape(tmp_path / "t", n_trades=5, n_quotes=3)
    bt, strat = _runner()
    stats = bt.run_tape(str(tmp_path / "t"))
    assert strat.trades == 5
    assert stats["skipped_option_quotes"] == 3
    assert stats["skipped_pool_states"] == 0

    again = bt.run_tape(str(tmp_path / "t"))
    assert again["skipped_option_quotes"] == 3, "counts are per run, not cumulative"
    assert again["skipped_pool_states"] == 0


def test_a_trade_only_tape_reports_zero(tmp_path):
    _tape(tmp_path / "t", n_trades=4, n_quotes=0)
    bt, _ = _runner()
    stats = bt.run_tape(str(tmp_path / "t"))
    assert stats["skipped_option_quotes"] == 0
    assert stats["skipped_pool_states"] == 0


def test_run_tapes_reports_the_counts_too(tmp_path):
    reg = lrvx.SymbolRegistry()
    sym = reg.add_symbol("bybit", "AAA", tick_size=0.01)
    hook = lrvx.BinaryLogRecorderHook(
        str(tmp_path / "t"), max_segment_mb=4, exchange_id=0, compression="none",
        exchange_name="bybit", instrument_type="perpetual")
    hook.add_symbol(sym, "AAA", "", "", 2, 6)
    rec = lrvx.Runner(reg, on_signal=lambda _s: None)
    rec.set_market_data_recorder(hook)
    rec.start()
    for i in range(3):
        rec.on_trade(sym, price=100.0, qty=0.1, is_buy=True, ts_ns=T0 + i * 1_000_000)
    rec.stop()

    bt, _ = _runner()
    stats = bt.run_tapes([str(tmp_path / "t")])
    assert stats["skipped_option_quotes"] == 0
    assert stats["skipped_pool_states"] == 0
