"""A signal's id in a .lrvxrun trace is the whole 64-bit id.

A signal a runner captures carries its order id, (subscriber id << 32) + n.
The record kept the low 32 bits, so every strategy's first order was signal 1.
"""
from __future__ import annotations

import lrvx

T0 = 1_788_220_800_000_000_000


def test_a_written_signal_reads_back_with_both_ids(tmp_path):
    path = str(tmp_path / "t.lrvxrun")
    rec = lrvx.TraceRecorder(path, strategy_id="ids")
    rec.write_signal(run_ts_ns=1, signal_id=42, name="limit", symbol_ids=[1])
    rec.close()

    (sig,) = lrvx.TraceReader(path).read_all_signals()
    assert sig["signal_id"] == 42
    assert sig["signal_id64"] == 42


def test_a_record_with_a_small_id_has_zero_reserved_bytes(tmp_path):
    path = tmp_path / "t.lrvxrun"
    rec = lrvx.TraceRecorder(str(path), strategy_id="ids")
    rec.write_signal(run_ts_ns=1, signal_id=0xFFFFFFFF, name="x", symbol_ids=[1])
    rec.close()
    raw = (path / "signals-000000.bin").read_bytes()
    record = raw[64 + 12:64 + 12 + 48]
    assert record[16:20] == b"\xff\xff\xff\xff"
    assert record[40:48] == bytes(8), "byte-identical to what was written before"


class OneOrder(lrvx.Strategy):
    def __init__(self, symbols):
        super().__init__(symbols)
        self.order_id = 0

    def on_trade(self, ctx, trade):
        if self.order_id == 0:
            self.order_id = self.limit_sell(101.0, 1.0)


def test_a_captured_signal_reads_back_as_its_order_id(tmp_path):
    path = str(tmp_path / "run.lrvxrun")
    reg = lrvx.SymbolRegistry()
    sym = reg.add_symbol("test", "BTCUSDT", tick_size=0.01)
    rec = lrvx.TraceRecorder(path, strategy_id="capture")
    runner = lrvx.Runner(reg, lambda s: None)
    strat = OneOrder([sym])
    runner.add_strategy(strat)
    runner.attach_trace_recorder(rec)
    runner.start()
    runner.on_trade(sym, 100.0, 1.0, True, T0)
    runner.stop()
    runner.attach_trace_recorder(None)
    rec.close()

    assert strat.order_id >= 1 << 32, "a strategy's order id carries its subscriber id"
    (sig,) = lrvx.TraceReader(path).read_all_signals()
    assert sig["signal_id64"] == strat.order_id
    assert sig["signal_id"] == strat.order_id & 0xFFFFFFFF
