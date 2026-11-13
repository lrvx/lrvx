"""What Runner.attach_trace_recorder captures by itself.

docs/how-to/lrvxrun-auto-capture.md describes this; the cases here are the
statements it makes. The executor is an in-process fake.
"""
from __future__ import annotations

import lrvx

T0 = 1_788_220_800_000_000_000


class Fake(lrvx.Executor):
    def __init__(self):
        super().__init__()
        self.submitted = []
        self.canceled = []

    def submit(self, order):
        self.submitted.append(order)

    def cancel(self, order_id):
        self.canceled.append(order_id)


class LimitThenCancel(lrvx.Strategy):
    def __init__(self, symbols):
        super().__init__(symbols)
        self.order_id = 0
        self.step = 0

    def on_trade(self, ctx, trade):
        self.step += 1
        if self.step == 1:
            self.order_id = self.limit_buy(99.0, 1.0)
        elif self.step == 2:
            self.cancel_order(self.order_id)


class DenyAll(lrvx.KillSwitch):
    def check(self, signal):
        return False


def _run(tmp_path, *, executor=None, kill_switch=None, trades=2):
    path = str(tmp_path / "run.lrvxrun")
    reg = lrvx.SymbolRegistry()
    sym = reg.add_symbol("test", "BTCUSDT", tick_size=0.01)
    rec = lrvx.TraceRecorder(path, strategy_id="auto")
    delivered = []
    runner = lrvx.Runner(reg, delivered.append)
    if executor is not None:
        runner.set_executor(executor)
    if kill_switch is not None:
        runner.set_kill_switch(kill_switch)
    strat = LimitThenCancel([sym])
    runner.add_strategy(strat)
    runner.attach_trace_recorder(rec)
    runner.start()
    for i in range(trades):
        runner.on_trade(sym, 100.0, 1.0, True, T0 + i)
    runner.stop()
    runner.attach_trace_recorder(None)
    rec.close()
    return lrvx.TraceReader(path), strat, delivered


def test_every_emitted_signal_is_captured_cancels_included(tmp_path):
    reader, strat, _ = _run(tmp_path)
    signals = reader.read_all_signals()
    assert [s["name"] for s in signals] == ["limit", "cancel"]
    assert [s["signal_id64"] for s in signals] == [strat.order_id, strat.order_id]
    assert signals[0]["flags"] == lrvx.SIGNAL_FLAG_ENTER
    assert signals[1]["flags"] == lrvx.SIGNAL_FLAG_EXIT


def test_no_order_event_or_fill_is_captured_even_with_an_executor(tmp_path):
    executor = Fake()
    reader, strat, _ = _run(tmp_path, executor=executor)
    assert len(executor.submitted) == 1 and executor.canceled == [strat.order_id], \
        "the orders did reach the executor"
    assert len(reader.read_all_signals()) == 2
    assert reader.read_all_order_events() == []
    assert reader.read_all_fills() == []


def test_a_signal_a_gate_drops_is_still_captured(tmp_path):
    executor = Fake()
    reader, _, delivered = _run(tmp_path, executor=executor, kill_switch=DenyAll(), trades=1)
    assert delivered == [] and executor.submitted == [], "the kill switch dropped it"
    assert [s["name"] for s in reader.read_all_signals()] == ["limit"]
