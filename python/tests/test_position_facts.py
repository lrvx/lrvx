"""SymbolContext.entry_price and SymbolContext.bars_in_position.

entry_price is the average entry price of the open position (None when
flat): adds weight it by quantity, a partial close leaves it, a fill that
flips the position sets it to that fill's price. bars_in_position counts the
bar callbacks the strategy received while holding the current position, the
bar on which the position is first seen counting as 1; it is 0 when flat,
restarts at 1 on a flip, and stays 0 on a replay without bars.

Scenario A (the same in tests/test_strategy_position_facts.cpp and the Node,
QuickJS, Codon and C API tests): bars open at 100 + 10 i, high open + 3, low
open - 2, close open + 1, so the open, high, low and close of every bar are
four different numbers. An order sent from on_bar fills at the next bar's
open, before that bar's on_bar.

    bar 1 buy 1   -> 120 at bar 2   long 1, entry 120
    bar 3 buy 1   -> 140 at bar 4   long 2, entry 130 (weighted)
    bar 5 sell 1  -> 160 at bar 6   long 1, entry 130 (FIFO would say 140)
    bar 7 sell 3  -> 180 at bar 8   short 2, entry 180, count restarts
    bar 9 buy 2   -> 200 at bar 10  flat
"""
from __future__ import annotations

import numpy as np
import pytest

import lrvx

SEC = 1_000_000_000
MIN = 60 * SEC
T0 = 1_767_225_600 * SEC  # 2026-01-01T00:00:00Z, a minute boundary

PLAN = {1: 1.0, 3: 1.0, 5: -1.0, 7: -3.0, 9: 2.0}
EXPECTED = [
    (None, 0), (None, 0), (120.0, 1), (120.0, 2), (130.0, 3), (130.0, 4),
    (130.0, 5), (130.0, 6), (180.0, 1), (180.0, 2), (None, 0), (None, 0),
]
POSITIONS = [0, 0, 1, 1, 2, 2, 1, 1, -2, -2, 0, 0]


class Scenario(lrvx.Strategy):
    def __init__(self, symbols, plan=PLAN):
        super().__init__(symbols)
        self.plan = plan
        self.i = 0
        self.seen = []  # (entry_price, bars_in_position, position)
        self.trades_seen = []

    def on_bar(self, ctx, bar):
        self.seen.append((ctx.entry_price, ctx.bars_in_position, ctx.position))
        qty = self.plan.get(self.i)
        if qty is not None:
            if qty > 0:
                self.market_buy(qty)
            else:
                self.market_sell(-qty)
        self.i += 1

    def on_trade(self, ctx, trade):
        self.trades_seen.append((ctx.entry_price, ctx.bars_in_position, ctx.position))


def _check(seen, label, expected=EXPECTED, positions=POSITIONS):
    assert [p for _, _, p in seen] == positions, f"{label}: premise, the fills land as planned"
    for i, ((entry, bars, _), (want_entry, want_bars)) in enumerate(zip(seen, expected)):
        if want_entry is None:
            assert entry is None, f"{label} bar {i}: entry_price {entry!r}, want None"
        else:
            assert entry is not None and entry == pytest.approx(want_entry, abs=1e-9), (
                f"{label} bar {i}: entry_price {entry!r}, want {want_entry}")
        assert bars == want_bars, f"{label} bar {i}: bars_in_position {bars}, want {want_bars}"
        assert isinstance(bars, int)


def _bars(n=12):
    start = np.arange(n, dtype=np.int64) * MIN
    end = start + MIN
    o = np.array([100.0 + 10.0 * i for i in range(n)])
    return start, end, o, o + 3.0, o - 2.0, o + 1.0, np.full(n, 100.0)


def _runner(fee_rate=0.0):
    reg = lrvx.SymbolRegistry()
    sym = reg.add_symbol("backtest", "BTCUSDT", tick_size=0.01)
    return sym, lrvx.BacktestRunner(reg, fee_rate=fee_rate, initial_capital=1_000_000.0)


def test_symbol_context_defaults_to_flat():
    ctx = lrvx.SymbolContext()
    assert ctx.entry_price is None
    assert ctx.bars_in_position == 0


@pytest.mark.parametrize("fee_rate", [0.0, 0.01], ids=["no-fee", "fee-1pct"])
def test_run_bars_weights_adds_keeps_partial_closes_and_restarts_on_flip(fee_rate):
    sym, runner = _runner(fee_rate)
    strat = Scenario([sym])
    runner.set_strategy(strat)
    runner.run_bars(*_bars(), symbol="BTCUSDT")
    _check(strat.seen, f"run_bars fee {fee_rate}")


def test_a_second_run_on_the_same_runner_starts_clean():
    sym, runner = _runner()
    strat = Scenario([sym])
    runner.set_strategy(strat)
    runner.run_bars(*_bars(4), symbol="BTCUSDT")
    # The first run ends long 1 at 120 with two bars held.
    assert strat.seen[3][0] == pytest.approx(120.0)
    assert strat.seen[3][1] == 2

    strat.seen.clear()
    strat.i = 0
    runner.run_bars(*_bars(), symbol="BTCUSDT")
    _check(strat.seen, "second run")


class Ticks(lrvx.Strategy):
    """Buys on the trades named in `buys` (trade index -> quantity)."""

    def __init__(self, symbols, buys):
        super().__init__(symbols)
        self.buys = buys
        self.seen = []
        self.fills = []

    def on_trade(self, ctx, trade):
        self.seen.append((ctx.entry_price, ctx.bars_in_position, ctx.position))
        qty = self.buys.get(len(self.seen) - 1)
        if qty:
            self.market_buy(qty)

    def on_fill(self, ctx, ev):
        self.fills.append((ev.fill_price, ev.fill_qty))


def test_tick_replay_sets_the_entry_but_counts_no_bars():
    sym, runner = _runner()
    strat = Ticks([sym], {0: 1.0, 2: 3.0})
    runner.set_strategy(strat)
    ts = np.arange(1, 7, dtype=np.int64) * SEC
    runner.run_ohlcv(ts, np.array([100.0, 104.0, 108.0, 112.0, 116.0, 120.0]), symbol="BTCUSDT")

    assert len(strat.fills) == 2
    (p1, q1), (p2, q2) = strat.fills
    assert p1 != p2, "premise: the two buys fill at different prices"
    assert strat.seen[0][0] is None
    assert all(bars == 0 for _, bars, _ in strat.seen), "no bars on a trade replay"
    assert strat.seen[-1][2] == 4.0
    for entry, _, pos in strat.seen:
        if pos == 1.0:
            assert entry == pytest.approx(p1, abs=1e-9)
    assert strat.seen[-1][0] == pytest.approx((p1 * q1 + p2 * q2) / (q1 + q2), abs=1e-9)


def test_run_csv_sets_the_entry_but_counts_no_bars(tmp_path):
    sym, runner = _runner()
    csv = tmp_path / "BTCUSDT.csv"
    rows = ["timestamp,open,high,low,close,volume"]
    for i, c in enumerate([100.0, 104.0, 108.0, 112.0, 116.0, 120.0]):
        rows.append(f"{(i + 1) * SEC},{c - 1},{c + 2},{c - 3},{c},10")
    csv.write_text("\n".join(rows) + "\n")
    strat = Ticks([sym], {1: 2.0})
    runner.set_strategy(strat)
    runner.run_csv(str(csv), symbol="BTCUSDT")
    assert len(strat.fills) == 1
    held = [s for s in strat.seen if s[2] != 0]
    assert held, "premise: the buy filled before the replay ended"
    for entry, bars, _ in held:
        assert entry == pytest.approx(strat.fills[0][0], abs=1e-9)
        assert bars == 0


def _write_tape(path, n_minutes=13):
    """Four prints per minute at the scenario's open, high, low and close,
    so run_tape_bars builds the scenario's bars; one more minute so the last
    scenario bar closes."""
    ts, px = [], []
    for i in range(n_minutes):
        o = 100.0 + 10.0 * i
        base = T0 + i * MIN
        for off, p in ((1, o), (10, o + 3.0), (20, o - 2.0), (50, o + 1.0)):
            ts.append(base + off * SEC)
            px.append(p)
    n = len(ts)
    w = lrvx.DataWriter(str(path))
    ts = np.asarray(ts, dtype=np.int64)
    w.write_trades(ts, ts, np.asarray(px, dtype=np.float64), np.full(n, 1.0),
                   np.arange(1, n + 1, dtype=np.uint64), np.full(n, 1, np.uint32),
                   np.ones(n, np.uint8))
    w.close()


@pytest.mark.parametrize("method", ["run_tape_bars", "run_tape_bars_on_ticks"])
def test_tape_bar_modes(tmp_path, method):
    tape = tmp_path / "tape"
    _write_tape(tape)
    reg = lrvx.SymbolRegistry()
    assert reg.add_symbol("test", "BTCUSDT", tick_size=0.01).id == 1
    runner = lrvx.BacktestRunner(reg, fee_rate=0.0, initial_capital=1_000_000.0)
    strat = Scenario([1])
    runner.set_strategy(strat)
    getattr(runner, method)(str(tape), interval_seconds=60.0, symbol="BTCUSDT")
    assert len(strat.seen) >= 12
    _check(strat.seen[:12], method)


def test_live_runner_fills_the_same_context():
    """The live path: bars pushed by hand, fills reported by the venue
    through on_order_event. Two symbols on one strategy keep their own
    facts."""
    reg = lrvx.SymbolRegistry()
    a = reg.add_symbol("test", "AAA", tick_size=0.01)
    b = reg.add_symbol("test", "BBB", tick_size=0.01)

    class Live(lrvx.Strategy):
        def __init__(self, symbols):
            super().__init__(symbols)
            self.seen = []

        def on_bar(self, ctx, bar):
            self.seen.append((ctx.symbol_id, ctx.entry_price, ctx.bars_in_position, ctx.position))

    strat = Live([a, b])
    runner = lrvx.Runner(reg, lambda s: None)
    runner.add_strategy(strat)
    runner.start()
    def fill(order_id, sym, side, qty, price):
        runner.on_order_event(order_id, sym, side, "FILLED", "market", qty, price)

    def bar(sym, i, o):
        runner.on_bar(sym, o, o + 3.0, o - 2.0, o + 1.0, 100.0, 50.0, T0 + i * MIN,
                      T0 + (i + 1) * MIN)

    try:
        bar(a, 0, 40.0)                                    # A flat
        fill(strat.market_buy(1.0, "AAA"), a, "buy", 1.0, 50.0)
        bar(a, 1, 50.0)                                    # A long 1 @ 50, 1
        bar(b, 1, 500.0)                                   # B flat
        fill(strat.market_buy(3.0, "AAA"), a, "buy", 3.0, 70.0)
        bar(a, 2, 70.0)                                    # A long 4 @ 65, 2
        fill(strat.market_sell(1.0, "BBB"), b, "sell", 1.0, 500.0)
        bar(b, 2, 510.0)                                   # B short 1 @ 500, 1
        fill(strat.market_sell(2.0, "AAA"), a, "sell", 2.0, 75.0)
        bar(a, 3, 75.0)                                    # A long 2 @ 65, 3
        fill(strat.market_sell(4.0, "AAA"), a, "sell", 4.0, 80.0)
        bar(a, 4, 80.0)                                    # A short 2 @ 80, 1
        fill(strat.market_buy(2.0, "AAA"), a, "buy", 2.0, 85.0)
        bar(a, 5, 85.0)                                    # A flat
    finally:
        runner.stop()

    aid, bid = a.id, b.id
    want = [
        (aid, None, 0, 0.0), (aid, 50.0, 1, 1.0), (bid, None, 0, 0.0), (aid, 65.0, 2, 4.0),
        (bid, 500.0, 1, -1.0), (aid, 65.0, 3, 2.0), (aid, 80.0, 1, -2.0), (aid, None, 0, 0.0),
    ]
    assert [(s, p) for s, _, _, p in strat.seen] == [(s, p) for s, _, _, p in want], "premise"
    for i, (got, exp) in enumerate(zip(strat.seen, want)):
        _, entry, bars, _ = got
        if exp[1] is None:
            assert entry is None, f"callback {i}: {entry!r}"
        else:
            assert entry == pytest.approx(exp[1], abs=1e-9), f"callback {i}: {entry!r}"
        assert bars == exp[2], f"callback {i}: bars_in_position {bars}"
