"""BacktestRunner.run_tape_bars_on_ticks: decide on bars, fill on ticks.

on_bar receives the bars run_tape_bars builds from the tape, while the
simulated executor is fed every trade as run_tape feeds it. The rule pinned
here: an order sent from on_bar for bar n can first match against the first
trade stamped at or after the end of bar n.
"""
from __future__ import annotations

import gc
import resource
import sys

import numpy as np
import pytest

import lrvx

SEC = 1_000_000_000
MS = 1_000_000
T0 = 1_767_225_600 * SEC  # 2026-01-01T00:00:00Z, a whole hour


def _write(path, ts, px, qty=None, sym=1):
    w = lrvx.DataWriter(str(path))
    ts = np.asarray(ts, dtype=np.int64)
    n = len(ts)
    qty = np.full(n, 1.0) if qty is None else np.asarray(qty, dtype=np.float64)
    w.write_trades(ts, ts, np.asarray(px, dtype=np.float64), qty,
                   np.arange(1, n + 1, dtype=np.uint64), np.full(n, sym, np.uint32),
                   np.ones(n, np.uint8))
    w.close()


def _walk(seed, n, start):
    rng = np.random.default_rng(seed)
    ts = start + np.cumsum(rng.integers(1, 4 * SEC, n)).astype(np.int64)
    px = np.round(100.0 + np.cumsum(rng.integers(-20, 21, n)) * 0.01, 2)
    qty = np.round(rng.uniform(0.1, 5.0, n), 1)
    return ts, px, qty


def _registry():
    reg = lrvx.SymbolRegistry()
    assert reg.add_symbol("test", "AAA", tick_size=0.01).id == 1
    assert reg.add_symbol("test", "BBB", tick_size=0.01).id == 2
    return reg


class Rec(lrvx.Strategy):
    """Logs every bar and fill; `plan(self, i, bar)` sends the orders."""

    def __init__(self, symbols, plan=None):
        super().__init__(symbols)
        self.bars = []
        self.fills = []
        self.trades = 0
        self.plan = plan

    def on_bar(self, ctx, bar):
        self.bars.append((bar.start_time_ns, bar.end_time_ns, bar.open, bar.high, bar.low,
                          bar.close, bar.volume, bar.buy_volume, bar.bar_type,
                          bar.bar_type_param))
        if self.plan:
            self.plan(self, len(self.bars) - 1, bar)

    def on_trade(self, ctx, trade):
        self.trades += 1

    def on_fill(self, ctx, ev):
        self.fills.append((ev.side, ev.fill_price, ev.fill_qty, ev.exchange_ts_ns))


def _run(tape, plan=None, method="run_tape_bars_on_ticks", interval=60.0, **runner_kw):
    bt = lrvx.BacktestRunner(_registry(), fee_rate=0.0, initial_capital=10_000.0, **runner_kw)
    strat = Rec([1], plan)
    bt.set_strategy(strat)
    stats = getattr(bt, method)(str(tape), interval_seconds=interval, symbol="AAA")
    return stats, strat, bt


def _buy_first(s, i, bar):
    if i == 0:
        s.market_buy(1.0)


def _boundary_tape(path, boundary_ts):
    _write(path, [T0 + SEC, T0 + 30 * SEC, T0 + 60 * SEC - 1, boundary_ts, boundary_ts + 500 * MS,
                  T0 + 125 * SEC],
           [100.0, 100.5, 101.0, 102.0, 103.0, 104.0])


@pytest.mark.parametrize("offset", [0, 1])
def test_order_from_on_bar_first_matches_the_first_trade_at_or_after_the_end(tmp_path, offset):
    boundary = T0 + 60 * SEC + offset
    _boundary_tape(tmp_path / "t", boundary)
    _, strat, _ = _run(tmp_path / "t", _buy_first)
    assert len(strat.bars) == 2, "the bar still open at the end is not delivered"
    assert strat.bars[0][5] == 101.0, "the print at end - 1 ns belongs to bar 0"
    assert strat.fills == [("buy", 102.0, 1.0, boundary)]
    assert strat.trades == 0, "on_trade does not fire in this mode"


def test_the_same_strategy_on_bar_replay_fills_at_the_next_open(tmp_path):
    _boundary_tape(tmp_path / "t", T0 + 60 * SEC)
    _, strat, _ = _run(tmp_path / "t", _buy_first, method="run_tape_bars")
    assert [f[1] for f in strat.fills] == [strat.bars[1][2]]


def test_no_lookahead(tmp_path):
    """Buy a bar that closed up, sell on the next: profitable only by seeing a bar early."""
    ts = [T0 + i * SEC for i in range(59)]
    px = [100.0] * 59
    ts += [T0 + 60 * SEC + i * SEC for i in range(60)]
    px += [round(100.0 + i * 100.0 / 59, 2) for i in range(60)]
    ts += [T0 + 120 * SEC + i * SEC for i in range(121)]
    px += [200.0] * 121
    _write(tmp_path / "t", ts, px)
    state = {"long": False}

    def plan(s, i, bar):
        if not state["long"] and bar.close > bar.open:
            s.market_buy(1.0)
            state["long"] = True
        elif state["long"]:
            s.market_sell(1.0)
            state["long"] = False

    stats, strat, _ = _run(tmp_path / "t", plan)
    assert [f[1] for f in strat.fills] == [200.0, 200.0]
    assert stats["net_pnl"] <= 0.0


def test_armed_stop_fills_on_the_crossing_print_not_the_bar_low(tmp_path):
    _write(tmp_path / "t", [T0 + 10 * SEC, T0 + 60 * SEC, T0 + 70 * SEC, T0 + 80 * SEC,
                            T0 + 90 * SEC, T0 + 125 * SEC],
           [100.0, 100.0, 96.0, 94.5, 90.0, 91.0])

    def plan(s, i, bar):
        if i == 0:
            s.market_buy(1.0)
            s.stop_market("sell", 95.0, 1.0)

    _, ticks, _ = _run(tmp_path / "t", plan)
    _, bars, _ = _run(tmp_path / "t", plan, method="run_tape_bars")
    assert [f[1] for f in ticks.fills] == [100.0, 94.5]
    assert ticks.fills[1][3] == T0 + 80 * SEC
    assert [f[1] for f in bars.fills][1] == 90.0


def test_submit_latency_moves_the_fill_by_milliseconds(tmp_path):
    _write(tmp_path / "t", [T0 + 10 * SEC, T0 + 60 * SEC, T0 + 60 * SEC + 4 * MS,
                            T0 + 60 * SEC + 5 * MS, T0 + 61 * SEC, T0 + 125 * SEC],
           [100.0, 101.0, 102.0, 103.0, 104.0, 105.0])
    _, strat, _ = _run(tmp_path / "t", _buy_first, submit_ack_latency_ns=5 * MS)
    assert strat.fills == [("buy", 103.0, 1.0, T0 + 60 * SEC + 5 * MS)]


def _schedule(s, i, bar):
    if i % 6 == 1:
        s.market_buy(0.5)
    elif i % 6 == 4:
        s.market_sell(0.5)


def test_bars_are_the_run_tape_bars_bars(tmp_path):
    for k in (2, 0, 1):
        _write(tmp_path / "t", *_walk(30 + k, 1500, T0 + k * 3600 * SEC))
    _, a, _ = _run(tmp_path / "t", method="run_tape_bars")
    _, b, _ = _run(tmp_path / "t")
    assert len(a.bars) > 100
    assert a.bars == b.bars


def test_market_orders_fill_as_tick_replay_fills_them_at_the_boundary_trade(tmp_path):
    _write(tmp_path / "t", *_walk(7, 4000, T0))
    stats, on_bars, bt = _run(tmp_path / "t", _schedule)

    class TickTwin(Rec):
        """Closes its own minute buckets and sends the same orders from on_trade."""

        def __init__(self, symbols):
            super().__init__(symbols)
            self.live = None
            self.n = 0

        def on_trade(self, ctx, trade):
            bucket = trade.timestamp_ns // (60 * SEC)
            if self.live is None:
                self.live = bucket
            elif bucket > self.live:
                _schedule(self, self.n, None)
                self.n += 1
                self.live = bucket

    rt = lrvx.BacktestRunner(_registry(), fee_rate=0.0, initial_capital=10_000.0)
    twin = TickTwin([1])
    rt.set_strategy(twin)
    tick_stats = rt.run_tape(str(tmp_path / "t"))
    assert twin.n == len(on_bars.bars) > 50
    assert len(on_bars.fills) > 10
    assert on_bars.fills == twin.fills
    for key in ("total_trades", "net_pnl", "final_capital"):
        assert stats[key] == tick_stats[key], key


def test_segment_file_order_does_not_matter(tmp_path):
    parts = [_walk(50 + k, 1200, T0 + k * 3600 * SEC) for k in range(3)]
    for k in (0, 1, 2):
        _write(tmp_path / "in_order", *parts[k])
    for k in (2, 0, 1):
        _write(tmp_path / "shuffled", *parts[k])
    first = [s["first_event_ns"] for s in lrvx.DataReader(str(tmp_path / "shuffled")).segments()]
    assert first != sorted(first)
    sa, a, ba = _run(tmp_path / "in_order", _schedule)
    sb, b, bb = _run(tmp_path / "shuffled", _schedule)
    assert sa == sb
    assert a.bars == b.bars and a.fills == b.fills and len(a.fills) > 10
    for k, v in ba.trades().items():
        assert np.array_equal(v, bb.trades()[k]), k
    for k, v in ba.equity_curve().items():
        assert np.array_equal(v, bb.equity_curve()[k]), k


def test_result_shape_matches_the_other_runners(tmp_path):
    _write(tmp_path / "t", *_walk(9, 2000, T0))
    stats, _, bt = _run(tmp_path / "t", _schedule)
    tape_stats, _, _ = _run(tmp_path / "t", method="run_tape_bars")
    assert set(stats) == set(tape_stats) | {"skipped_option_quotes", "skipped_pool_states"}
    assert stats["skipped_option_quotes"] == 0 and stats["skipped_pool_states"] == 0
    assert stats["total_trades"] > 0
    assert set(bt.trades()) and bt.trades()["entry_price"].size == stats["total_trades"]
    assert bt.equity_curve()["equity"].size > 0


def test_skipped_option_quotes_are_counted(tmp_path):
    ts, px, qty = _walk(4, 300, T0)
    w = lrvx.DataWriter(str(tmp_path / "t"))
    w.write_trades(ts, ts, px, qty, np.arange(1, 301, dtype=np.uint64),
                   np.ones(300, np.uint32), np.ones(300, np.uint8))
    qts = ts[:4] + 1
    w.write_option_quotes(exchange_ts_ns=qts, recv_ts_ns=qts, mark_prices=np.full(4, 0.05),
                          index_prices=np.full(4, 100.0), ivs=np.full(4, 0.6),
                          open_interest=np.full(4, 10.0), symbol_ids=np.full(4, 1, np.uint32))
    w.close()
    stats, _, _ = _run(tmp_path / "t")
    assert stats["skipped_option_quotes"] == 4
    assert stats["skipped_pool_states"] == 0


def test_errors(tmp_path):
    _write(tmp_path / "t", *_walk(1, 100, T0), sym=1)
    _write(tmp_path / "t", *_walk(2, 100, T0), sym=2)
    bt = lrvx.BacktestRunner(_registry())
    with pytest.raises(lrvx.LrvxError):
        bt.run_tape_bars_on_ticks(str(tmp_path / "t"), interval_seconds=60.0, symbol="AAA")
    bt.set_strategy(Rec([1]))
    with pytest.raises(lrvx.LrvxError, match="more than one symbol"):
        bt.run_tape_bars_on_ticks(str(tmp_path / "t"), interval_seconds=60.0)
    for bad in (0.0, -1.0, float("nan"), float("inf")):
        with pytest.raises(lrvx.LrvxError, match="interval_seconds"):
            bt.run_tape_bars_on_ticks(str(tmp_path / "t"), interval_seconds=bad, symbol="AAA")
    reg = _registry()
    reg.add_symbol("test", "CCC", tick_size=0.01)
    bt3 = lrvx.BacktestRunner(reg)
    bt3.set_strategy(Rec([3]))
    with pytest.raises(lrvx.LrvxError, match="CCC"):
        bt3.run_tape_bars_on_ticks(str(tmp_path / "t"), interval_seconds=60.0, symbol="CCC")
    with pytest.raises(Exception, match="does not exist"):
        bt.run_tape_bars_on_ticks(str(tmp_path / "nope"), interval_seconds=60.0, symbol="AAA")


def test_trades_are_streamed_not_materialised(tmp_path):
    """Peak RSS stays flat over a 3M-trade tape."""
    if sys.platform not in ("darwin", "linux"):
        pytest.skip("ru_maxrss semantics")
    tape = tmp_path / "big"
    rng = np.random.default_rng(7)
    t = T0
    for _ in range(30):
        n = 100_000
        w = lrvx.DataWriter(str(tape))
        ts = t + np.cumsum(rng.integers(1_000_000, 50_000_000, n)).astype(np.int64)
        t = int(ts[-1]) + 1
        px = np.round(100.0 + np.cumsum(rng.integers(-3, 4, n)) * 0.01, 2)
        w.write_trades(ts, ts, px, np.full(n, 0.01), np.arange(n, dtype=np.uint64),
                       np.ones(n, np.uint32), np.zeros(n, np.uint8))
        w.close()
        del ts, px
    gc.collect()
    scale = 1 if sys.platform == "darwin" else 1024
    before = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss * scale
    stats, strat, _ = _run(tape, _schedule)
    after = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss * scale
    assert len(strat.bars) > 1000
    assert stats["total_trades"] > 100
    assert after - before < 12 * 1024 * 1024, (before, after)
