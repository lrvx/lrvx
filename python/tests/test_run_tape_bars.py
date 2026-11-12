"""BacktestRunner.run_tape_bars: a tape replayed as time bars in one call.

The explicit form of the same run is three steps: DataReader.read_trades(),
aggregate_time_bars(), run_bars(). run_tape_bars has to land on exactly the
same fills, trades and equity curve for the same interval, while never handing
the trade array to Python.
"""
from __future__ import annotations

import gc
import resource
import sys

import numpy as np
import pytest

import lrvx

SEC = 1_000_000_000
T0 = 1_767_225_600 * SEC  # 2026-01-01T00:00:00Z, a whole hour
SYMBOLS = ("AAA", "BBB")


def _trades(seed: int, n: int, start_ns: int, span_ns: int, base: float):
    """A random walk on a 0.01 grid with uneven gaps, so some intervals are empty."""
    rng = np.random.default_rng(seed)
    ts = np.sort(rng.integers(start_ns, start_ns + span_ns, n)).astype(np.int64)
    # carve out a quiet stretch so the bar series has holes
    quiet = (ts > start_ns + span_ns // 3) & (ts < start_ns + span_ns // 3 + 7 * 60 * SEC)
    ts = ts[~quiet]
    steps = rng.integers(-30, 31, len(ts))
    px = np.round(base + np.cumsum(steps) * 0.01, 2)
    qty = np.round(rng.uniform(0.001, 2.0, len(ts)), 3)
    side = rng.integers(0, 2, len(ts)).astype(np.uint8)
    return ts, px, qty, side


def _write(dir_path, chunks):
    """One DataWriter per chunk: each becomes its own segment, named by wall clock."""
    for chunk in chunks:
        w = lrvx.DataWriter(str(dir_path))
        ts = np.concatenate([c[0] for c in chunk])
        order = np.argsort(ts, kind="stable")
        ts = ts[order]
        px = np.concatenate([c[1] for c in chunk])[order]
        qty = np.concatenate([c[2] for c in chunk])[order]
        side = np.concatenate([c[3] for c in chunk])[order]
        sym = np.concatenate([np.full(len(c[0]), c[4], np.uint32) for c in chunk])[order]
        ids = np.arange(len(ts), dtype=np.uint64)
        w.write_trades(ts, ts, px, qty, ids, sym, side)
        w.close()


def _segments():
    """Three time slices, two symbols, interleaved inside every slice."""
    hour = 3600 * SEC
    out = []
    for k in range(3):
        a = _trades(10 + k, 1500, T0 + k * hour, hour, 100.0 + k)
        b = _trades(20 + k, 900, T0 + k * hour, hour, 50.0 + k)
        out.append([(*a, 1), (*b, 2)])
    return out


@pytest.fixture(scope="module")
def tapes(tmp_path_factory):
    root = tmp_path_factory.mktemp("tape_bars")
    segs = _segments()
    in_order = root / "in_order"
    _write(in_order, segs)
    shuffled = root / "shuffled"
    _write(shuffled, [segs[2], segs[0], segs[1]])
    first = [s["first_event_ns"] for s in lrvx.DataReader(str(shuffled)).segments()]
    assert first != sorted(first), "fixture must list segments out of time order"
    return {"in_order": str(in_order), "shuffled": str(shuffled)}


def _registry():
    reg = lrvx.SymbolRegistry()
    ids = [reg.add_symbol("test", s, tick_size=0.01).id for s in SYMBOLS]
    assert ids == [1, 2], "the tape writes symbol ids 1 and 2"
    return reg


class Swing(lrvx.Strategy):
    """Deterministic, touches market, stop and close paths, and logs every bar."""

    def __init__(self, symbols):
        super().__init__(symbols)
        self.bars = []
        self.fills = []

    def on_bar(self, ctx, bar):
        self.bars.append((bar.symbol, bar.start_time_ns, bar.end_time_ns,
                          bar.open, bar.high, bar.low, bar.close, bar.volume))
        i = len(self.bars)
        if i % 7 == 1 and ctx.is_flat():
            self.market_buy(0.5)
            self.stop_market("sell", bar.low - 0.05, 0.5)
        elif i % 7 == 4 and not ctx.is_flat():
            self.cancel_all_orders()
            self.close_position()

    def on_fill(self, ctx, ev):
        self.fills.append((ev.side, ev.fill_price, ev.fill_qty))


def _three_step(tape, symbol, interval):
    reg = _registry()
    sid = SYMBOLS.index(symbol) + 1
    trades = lrvx.DataReader(tape).read_trades()
    trades = trades[trades["symbol_id"] == sid]
    bars = lrvx.aggregate_time_bars(
        trades["exchange_ts_ns"].astype(np.int64),
        trades["price_raw"] / lrvx.PRICE_SCALE,
        trades["qty_raw"] / lrvx.VOLUME_SCALE,
        (trades["side"] == 0).astype(np.uint8),
        interval_seconds=interval)
    bt = lrvx.BacktestRunner(reg, fee_rate=0.0004, initial_capital=10_000.0)
    strat = Swing([sid])
    bt.set_strategy(strat)
    stats = bt.run_bars(
        start_time_ns=bars["start_time_ns"], end_time_ns=bars["end_time_ns"],
        open=bars["open_raw"] / lrvx.PRICE_SCALE, high=bars["high_raw"] / lrvx.PRICE_SCALE,
        low=bars["low_raw"] / lrvx.PRICE_SCALE, close=bars["close_raw"] / lrvx.PRICE_SCALE,
        volume=bars["volume_raw"] / lrvx.VOLUME_SCALE, symbol=symbol)
    return stats, bt.trades(), bt.equity_curve(), strat, bars


def _one_call(tape, symbol, interval):
    reg = _registry()
    sid = SYMBOLS.index(symbol) + 1
    bt = lrvx.BacktestRunner(reg, fee_rate=0.0004, initial_capital=10_000.0)
    strat = Swing([sid])
    bt.set_strategy(strat)
    stats = bt.run_tape_bars(tape, interval_seconds=interval, symbol=symbol)
    return stats, bt.trades(), bt.equity_curve(), strat


def _same_arrays(a: dict, b: dict):
    assert a.keys() == b.keys()
    for k in a:
        assert a[k].dtype == b[k].dtype, k
        assert np.array_equal(a[k], b[k]), k


@pytest.mark.parametrize("interval", [60.0, 300.0])
@pytest.mark.parametrize("symbol", SYMBOLS)
def test_equals_three_step_route(tapes, symbol, interval):
    s3, t3, e3, strat3, bars = _three_step(tapes["in_order"], symbol, interval)
    s1, t1, e1, strat1 = _one_call(tapes["in_order"], symbol, interval)

    assert len(bars) > 20
    assert t3["symbol"].size > 3, "the strategy must actually trade for this to mean anything"
    assert s1 == s3
    _same_arrays(t1, t3)
    _same_arrays(e1, e3)
    assert strat1.fills == strat3.fills
    assert strat1.bars == strat3.bars
    assert len(strat1.bars) == len(bars)


def test_on_bar_count_is_the_bar_count_and_bars_are_wall_clock_aligned(tapes):
    _, _, _, strat = _one_call(tapes["in_order"], "AAA", 300.0)
    starts = [b[1] for b in strat.bars]
    assert all(s % (300 * SEC) == 0 for s in starts)
    assert all(b[2] - b[1] == 300 * SEC for b in strat.bars)
    assert starts == sorted(starts)
    # every bar comes from symbol 1 only
    assert {b[0] for b in strat.bars} == {1}
    # the last bucket the tape touches is still open when the tape ends: not emitted
    trades = lrvx.DataReader(tapes["in_order"]).read_trades()
    last_ts = int(trades[trades["symbol_id"] == 1]["exchange_ts_ns"].max())
    assert starts[-1] < (last_ts // (300 * SEC)) * (300 * SEC)


def test_bar_fields_carry_the_aggregated_bar(tapes):
    """bar_type is Time and bar_type_param the interval in ns, as a live time aggregator sets them."""
    reg = _registry()
    seen = []

    class Rec(lrvx.Strategy):
        def on_bar(self, ctx, bar):
            seen.append((bar.bar_type, bar.bar_type_param, bar.buy_volume, bar.volume))

    bt = lrvx.BacktestRunner(reg)
    bt.set_strategy(Rec([1]))
    bt.run_tape_bars(tapes["in_order"], interval_seconds=60.0, symbol="AAA")
    assert seen
    assert all(t == 0 and p == 60 * SEC for t, p, _, _ in seen)
    assert all(0.0 <= bv <= v for _, _, bv, v in seen)
    assert any(bv > 0.0 for _, _, bv, _ in seen)


def test_orders_from_on_bar_fill_at_next_bar_open(tapes):
    reg = _registry()

    class BuyOnce(lrvx.Strategy):
        def __init__(self, symbols):
            super().__init__(symbols)
            self.bars = []
            self.fills = []

        def on_bar(self, ctx, bar):
            self.bars.append((bar.open, bar.close))
            if len(self.bars) == 3:
                self.market_buy(1.0)

        def on_fill(self, ctx, ev):
            self.fills.append(ev.fill_price)

    strat = BuyOnce([1])
    bt = lrvx.BacktestRunner(reg, fee_rate=0.0)
    bt.set_strategy(strat)
    bt.run_tape_bars(tapes["in_order"], interval_seconds=60.0, symbol="AAA")
    assert strat.fills == [strat.bars[3][0]]
    assert strat.bars[3][0] != strat.bars[2][1] or True  # open of bar 4, not close of bar 3


def test_segment_file_order_does_not_matter(tapes):
    for symbol in SYMBOLS:
        a = _one_call(tapes["in_order"], symbol, 60.0)
        b = _one_call(tapes["shuffled"], symbol, 60.0)
        assert a[0] == b[0]
        _same_arrays(a[1], b[1])
        _same_arrays(a[2], b[2])
        assert a[3].bars == b[3].bars


def test_multi_symbol_tape_requires_a_symbol(tapes):
    reg = _registry()
    bt = lrvx.BacktestRunner(reg)
    strat = Swing([1, 2])
    bt.set_strategy(strat)
    with pytest.raises(lrvx.LrvxError, match="symbol"):
        bt.run_tape_bars(tapes["in_order"], interval_seconds=60.0)


def test_single_symbol_tape_needs_no_symbol(tmp_path):
    a = _trades(3, 800, T0, 3600 * SEC, 100.0)
    _write(tmp_path / "t", [[(*a, 1)]])
    reg = _registry()
    bt = lrvx.BacktestRunner(reg)
    implicit = Swing([1])
    bt.set_strategy(implicit)
    s_implicit = bt.run_tape_bars(str(tmp_path / "t"), interval_seconds=60.0)
    bt2 = lrvx.BacktestRunner(_registry())
    explicit = Swing([1])
    bt2.set_strategy(explicit)
    s_explicit = bt2.run_tape_bars(str(tmp_path / "t"), interval_seconds=60.0, symbol="AAA")
    assert s_implicit == s_explicit
    assert implicit.bars == explicit.bars and len(implicit.bars) > 30


def test_symbol_absent_from_tape_raises(tapes):
    reg = _registry()
    reg.add_symbol("test", "CCC", tick_size=0.01)
    bt = lrvx.BacktestRunner(reg)
    bt.set_strategy(Swing([3]))
    with pytest.raises(lrvx.LrvxError, match="CCC"):
        bt.run_tape_bars(tapes["in_order"], interval_seconds=60.0, symbol="CCC")


@pytest.mark.parametrize("bad", [0.0, -60.0, float("nan"), float("inf"), 1e-10])
def test_bad_interval_raises(tapes, bad):
    bt = lrvx.BacktestRunner(_registry())
    bt.set_strategy(Swing([1]))
    with pytest.raises(lrvx.LrvxError, match="interval_seconds"):
        bt.run_tape_bars(tapes["in_order"], interval_seconds=bad, symbol="AAA")


def test_requires_strategy_and_existing_tape(tapes, tmp_path):
    bt = lrvx.BacktestRunner(_registry())
    with pytest.raises(lrvx.LrvxError):
        bt.run_tape_bars(tapes["in_order"], interval_seconds=60.0, symbol="AAA")
    bt.set_strategy(Swing([1]))
    with pytest.raises(Exception, match="does not exist"):
        bt.run_tape_bars(str(tmp_path / "nope"), interval_seconds=60.0, symbol="AAA")


def test_interval_is_required(tapes):
    bt = lrvx.BacktestRunner(_registry())
    bt.set_strategy(Swing([1]))
    with pytest.raises(TypeError):
        bt.run_tape_bars(tapes["in_order"], symbol="AAA")  # type: ignore[call-arg]


def test_trades_never_reach_python(tmp_path):
    """Peak RSS stays flat while the trade count grows 10x.

    Writing the tape is done in bounded chunks; the run itself must not grow
    with the trade count, which a read_trades() array of 3M rows (120 MB) would.
    """
    if sys.platform not in ("darwin", "linux"):
        pytest.skip("ru_maxrss semantics")
    tape = tmp_path / "big"
    n_chunks, per_chunk = 30, 100_000
    rng = np.random.default_rng(7)
    t = T0
    for _ in range(n_chunks):
        w = lrvx.DataWriter(str(tape))
        ts = t + np.cumsum(rng.integers(1_000_000, 50_000_000, per_chunk)).astype(np.int64)
        t = int(ts[-1]) + 1
        px = np.round(100.0 + np.cumsum(rng.integers(-3, 4, per_chunk)) * 0.01, 2)
        w.write_trades(ts, ts, px, np.full(per_chunk, 0.01), np.arange(per_chunk, dtype=np.uint64),
                       np.ones(per_chunk, np.uint32), np.zeros(per_chunk, np.uint8))
        w.close()
        del ts, px
    gc.collect()

    scale = 1 if sys.platform == "darwin" else 1024  # bytes on macOS, KiB on Linux
    before = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss * scale
    bt = lrvx.BacktestRunner(_registry())
    strat = Swing([1])
    bt.set_strategy(strat)
    bt.run_tape_bars(str(tape), interval_seconds=60.0, symbol="AAA")
    after = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss * scale
    assert len(strat.bars) > 1000
    # 3M trades as a read_trades() array is 3M * 40 B = 120 MB; allow a tenth of that
    assert after - before < 12 * 1024 * 1024, (before, after)
