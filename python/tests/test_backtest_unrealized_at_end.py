"""The open position at the end of a backtest, valued.

``net_pnl``, ``total_pnl``, ``total_fees``, ``trades()`` and
``equity_curve()`` keep their meaning (closed trades; every fill's fee, see
test_backtest_open_position_at_end.py). The stats also carry the position
still open when the data ends, marked at the last price the run saw for its
symbol -- a bar's close on bar replay, the last trade on tick replay:

* ``unrealized_pnl``: sum over symbols of ``(mark - entry) * qty`` for a
  long and ``(entry - mark) * qty`` for a short, no exit fee;
* ``final_equity``: ``initial_capital + net_pnl + unrealized_pnl``.

``BacktestRunner.open_positions()`` lists each open position as a dict of
numpy arrays aligned by row, like ``trades()``: ``symbol``, ``side`` (0 long,
1 short), ``quantity``, ``entry_price`` (average), ``mark_price``,
``unrealized_pnl``; ascending symbol id, flat symbols left out.
``BacktestResult`` takes the mark by hand with ``mark_price(symbol, price)``
(an unmarked symbol is valued at its last fill) and has the same
``open_positions()`` as a structured array, and the same two stats keys.
"""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
for cand in ("build/python", "build-py312/python"):
    p = REPO_ROOT / cand
    if p.is_dir():
        sys.path.insert(0, str(p))
        break

import lrvx  # noqa: E402

FEE = 0.0004
CAPITAL = 10_000.0
SEC = 1_000_000_000
MIN = 60 * SEC
T0 = 1_767_225_600 * SEC  # 2026-01-01T00:00:00Z


def _runner(*names):
    reg = lrvx.SymbolRegistry()
    syms = [reg.add_symbol("backtest", n, tick_size=0.01) for n in (names or ("BTCUSDT",))]
    return lrvx.BacktestRunner(reg, fee_rate=FEE, initial_capital=CAPITAL), syms, reg


def _equity_identity(stats):
    assert stats["final_equity"] == pytest.approx(
        stats["initial_capital"] + stats["net_pnl"] + stats["unrealized_pnl"], abs=1e-9)


class _TradeOnce(lrvx.Strategy):
    """Sends one market order on the given trade of each listed symbol:
    {symbol id: (trade index of that symbol, signed qty)}."""

    def __init__(self, symbols, orders):
        super().__init__(symbols)
        self.orders = {int(k): v for k, v in orders.items()}
        self.seen = {}

    def on_trade(self, ctx, trade):
        sym = int(ctx.symbol_id)
        i = self.seen.get(sym, 0)
        self.seen[sym] = i + 1
        if sym in self.orders and self.orders[sym][0] == i:
            qty = self.orders[sym][1]
            if qty > 0:
                self.emit_market_buy(sym, qty)
            else:
                self.emit_market_sell(sym, -qty)


class _BarOrders(lrvx.Strategy):
    """Sends a market order on the given bars: {bar index: signed qty}."""

    def __init__(self, symbols, orders):
        super().__init__(symbols)
        self.orders = dict(orders)
        self.n = 0

    def on_bar(self, ctx, bar):
        qty = self.orders.get(self.n)
        if qty:
            self.market_buy(qty) if qty > 0 else self.market_sell(-qty)
        self.n += 1


# ── run_ohlcv: tick replay, the mark is the last close ────────────────


@pytest.mark.parametrize("qty", [0.5, -0.5])
def test_run_ohlcv_ending_open_is_marked_at_the_last_close(qty):
    runner, syms, _ = _runner()
    closes = [100.0, 100.0, 100.0, 104.0, 107.0]
    runner.set_strategy(_TradeOnce(syms, {syms[0]: (0, qty)}))
    ts = np.array([T0 + i * MIN for i in range(len(closes))], dtype=np.int64)
    stats = runner.run_ohlcv(ts, np.array(closes), symbol="BTCUSDT")

    # Every price before the last two is 100, so the entry is 100 whichever
    # row the order fills on.
    open_ = runner.open_positions()
    assert list(open_["symbol"]) == [int(syms[0])]
    assert list(open_["side"]) == [0 if qty > 0 else 1]
    assert open_["quantity"][0] == pytest.approx(0.5)
    assert open_["entry_price"][0] == pytest.approx(100.0)
    assert open_["mark_price"][0] == pytest.approx(107.0)
    expected = 3.5 if qty > 0 else -3.5
    assert open_["unrealized_pnl"][0] == pytest.approx(expected, abs=1e-9)
    assert stats["unrealized_pnl"] == pytest.approx(expected, abs=1e-9)
    _equity_identity(stats)


def test_run_ohlcv_ending_flat_has_no_unrealized():
    runner, syms, _ = _runner()

    class RoundTrip(lrvx.Strategy):
        def __init__(self, symbols):
            super().__init__(symbols)
            self.n = 0

        def on_trade(self, ctx, trade):
            if self.n == 0:
                self.market_buy(1.0)
            elif self.n == 2:
                self.close_position()
            self.n += 1

    runner.set_strategy(RoundTrip(syms))
    closes = [100.0, 101.0, 102.0, 103.0, 150.0]
    ts = np.array([T0 + i * MIN for i in range(len(closes))], dtype=np.int64)
    stats = runner.run_ohlcv(ts, np.array(closes), symbol="BTCUSDT")
    assert stats["total_trades"] == 1
    assert stats["unrealized_pnl"] == 0.0
    assert stats["final_equity"] == pytest.approx(stats["final_capital"], abs=1e-9)
    assert len(runner.open_positions()["symbol"]) == 0


def test_the_existing_numbers_do_not_change_with_an_open_position():
    """net_pnl keeps the open entry's fee and leaves out its unrealized pnl."""
    runner, syms, _ = _runner()

    class BuySellBuy(lrvx.Strategy):
        def __init__(self, symbols):
            super().__init__(symbols)
            self.n = 0

        def on_trade(self, ctx, trade):
            if self.n in (0, 4):
                self.market_buy(0.01)
            elif self.n == 2:
                self.close_position()
            self.n += 1

    runner.set_strategy(BuySellBuy(syms))
    closes = [78_000.0, 78_500.0, 79_000.0, 78_000.0, 78_000.0, 78_000.0, 78_000.0, 80_000.0]
    ts = np.array([T0 + i * MIN for i in range(len(closes))], dtype=np.int64)
    stats = runner.run_ohlcv(ts, np.array(closes), symbol="BTCUSDT")
    trades = runner.trades()
    closed_net = float(sum(trades["pnl"]) - sum(trades["fee"]))
    open_fee = 0.01 * 78_000.0 * FEE
    assert stats["net_pnl"] == pytest.approx(closed_net - open_fee, abs=1e-9)
    assert stats["final_capital"] == pytest.approx(CAPITAL + closed_net - open_fee, abs=1e-9)
    # (80,000 - 78,000) * 0.01
    assert stats["unrealized_pnl"] == pytest.approx(20.0, abs=1e-9)
    assert stats["final_equity"] == pytest.approx(CAPITAL + closed_net - open_fee + 20.0,
                                                  abs=1e-9)
    assert len(runner.equity_curve()["equity"]) == 1


def test_no_closed_trade_still_values_the_open_position():
    runner, syms, _ = _runner()
    runner.set_strategy(_TradeOnce(syms, {syms[0]: (0, 1.0)}))
    closes = [100.0, 100.0, 96.0]
    ts = np.array([T0 + i * MIN for i in range(len(closes))], dtype=np.int64)
    stats = runner.run_ohlcv(ts, np.array(closes), symbol="BTCUSDT")
    assert stats["total_trades"] == 0
    assert stats["net_pnl"] == 0.0
    assert stats["unrealized_pnl"] == pytest.approx(-4.0, abs=1e-9)
    _equity_identity(stats)


# ── run_bars: the mark is the last bar's close ───────────────────────


@pytest.mark.parametrize("qty,expected", [(2.0, -8.0), (-2.0, 8.0)])
def test_run_bars_is_marked_at_the_last_bar_close(qty, expected):
    runner, syms, _ = _runner()
    runner.set_strategy(_BarOrders(syms, {0: qty}))
    start = np.array([T0, T0 + MIN, T0 + 2 * MIN], dtype=np.int64)
    stats = runner.run_bars(start, start + MIN,
                            np.array([100.0, 101.0, 103.0]), np.array([100.5, 103.0, 108.0]),
                            np.array([99.5, 100.0, 96.0]), np.array([100.0, 102.0, 97.0]),
                            np.array([10.0, 10.0, 10.0]), symbol="BTCUSDT")
    open_ = runner.open_positions()
    # Filled at bar 1's open (101), marked at bar 2's close (97).
    assert open_["entry_price"][0] == pytest.approx(101.0)
    assert open_["mark_price"][0] == pytest.approx(97.0)
    assert stats["unrealized_pnl"] == pytest.approx(expected, abs=1e-9)
    _equity_identity(stats)


# ── Tapes: bar replay vs tick replay ──────────────────────────────────


def _write_tape(path: Path, rows):
    """rows: (ts_ns, price, symbol_id)."""
    w = lrvx.DataWriter(str(path))
    ts = np.array([r[0] for r in rows], dtype=np.int64)
    w.write_trades(exchange_ts_ns=ts, recv_ts_ns=ts,
                   prices=np.array([r[1] for r in rows], dtype=np.float64),
                   quantities=np.ones(len(rows)),
                   trade_ids=np.arange(1, len(rows) + 1, dtype=np.uint64),
                   symbol_ids=np.array([r[2] for r in rows], dtype=np.uint32),
                   sides=np.ones(len(rows), dtype=np.uint8))
    w.close()


# Bar 0: 100, 101. Bar 1: 102, 104.5, 101.5, 103 (open 102, high 104.5,
# low 101.5, close 103). Bar 2 (still open at the end, never replayed as a
# bar): 106, 110.
_THREE_MINUTES = [(T0 + 1 * SEC, 100.0), (T0 + 30 * SEC, 101.0), (T0 + 61 * SEC, 102.0),
                  (T0 + 75 * SEC, 104.5), (T0 + 80 * SEC, 101.5), (T0 + 90 * SEC, 103.0),
                  (T0 + 121 * SEC, 106.0), (T0 + 150 * SEC, 110.0)]


def test_run_tape_bars_marks_at_the_last_replayed_bar_close(tmp_path):
    tape = tmp_path / "tape"
    _write_tape(tape, [(t, p, 1) for t, p in _THREE_MINUTES])
    runner, syms, _ = _runner()
    runner.set_strategy(_BarOrders(syms, {0: 1.0}))
    stats = runner.run_tape_bars(str(tape), interval_seconds=60, symbol="BTCUSDT")
    open_ = runner.open_positions()
    assert open_["entry_price"][0] == pytest.approx(102.0)
    assert open_["mark_price"][0] == pytest.approx(103.0), \
        "the bar still open at the end is not replayed, so its trades are not seen"
    assert stats["unrealized_pnl"] == pytest.approx(1.0, abs=1e-9)
    _equity_identity(stats)


def test_run_tape_bars_on_ticks_marks_at_the_last_trade(tmp_path):
    tape = tmp_path / "tape"
    _write_tape(tape, [(t, p, 1) for t, p in _THREE_MINUTES])
    runner, syms, _ = _runner()
    runner.set_strategy(_BarOrders(syms, {0: 1.0}))
    stats = runner.run_tape_bars_on_ticks(str(tape), interval_seconds=60, symbol="BTCUSDT")
    open_ = runner.open_positions()
    assert open_["entry_price"][0] == pytest.approx(102.0)
    assert open_["mark_price"][0] == pytest.approx(110.0)
    assert stats["unrealized_pnl"] == pytest.approx(8.0, abs=1e-9)
    _equity_identity(stats)


def test_run_tape_sums_open_positions_across_symbols(tmp_path):
    tape = tmp_path / "tape"
    _write_tape(tape, [(T0 + 1 * SEC, 100.0, 1), (T0 + 2 * SEC, 50.0, 2),
                       (T0 + 3 * SEC, 100.0, 1), (T0 + 4 * SEC, 50.0, 2),
                       (T0 + 5 * SEC, 104.0, 1), (T0 + 6 * SEC, 48.0, 2)])
    runner, syms, _ = _runner("BTCUSDT", "ETHUSDT")
    runner.set_strategy(_TradeOnce(syms, {syms[0]: (0, 1.0), syms[1]: (0, -2.0)}))
    stats = runner.run_tape(str(tape))
    open_ = runner.open_positions()
    assert list(open_["symbol"]) == [int(s) for s in syms]
    assert list(open_["side"]) == [0, 1]
    assert list(open_["mark_price"]) == pytest.approx([104.0, 48.0])
    # Entries are 100 and 50 whichever trade the orders fill on.
    assert list(open_["unrealized_pnl"]) == pytest.approx([4.0, 4.0])
    assert stats["unrealized_pnl"] == pytest.approx(8.0, abs=1e-9)
    _equity_identity(stats)


# ── BacktestResult by hand ───────────────────────────────────────────


def test_backtest_result_mark_price_and_open_positions():
    r = lrvx.BacktestResult(initial_capital=CAPITAL, fee_rate=FEE)
    r.record_fill(1, 1, "buy", 100.0, 1.0, T0)
    r.record_fill(2, 1, "sell", 110.0, 1.0, T0 + SEC)
    r.record_fill(3, 1, "buy", 105.0, 2.0, T0 + 2 * SEC)
    r.record_fill(4, 2, "sell", 50.0, 3.0, T0 + 3 * SEC)
    r.mark_price(1, 108.5)  # +7
    r.mark_price(2, 52.0)   # -6
    s = r.stats()
    assert s["unrealized_pnl"] == pytest.approx(1.0, abs=1e-9)
    _equity_identity(s)
    open_ = r.open_positions()
    assert list(open_["symbol"]) == [1, 2]
    assert list(open_["side"]) == [0, 1]
    assert list(open_["quantity"]) == pytest.approx([2.0, 3.0])
    assert list(open_["entry_price"]) == pytest.approx([105.0, 50.0])
    assert list(open_["mark_price"]) == pytest.approx([108.5, 52.0])
    assert list(open_["unrealized_pnl"]) == pytest.approx([7.0, -6.0])


def test_backtest_result_unmarked_symbol_uses_its_last_fill():
    r = lrvx.BacktestResult(initial_capital=CAPITAL, fee_rate=0.0)
    r.record_fill(1, 1, "buy", 100.0, 2.0, T0)
    r.record_fill(2, 1, "sell", 104.0, 1.0, T0 + SEC)
    assert r.stats()["unrealized_pnl"] == pytest.approx(4.0, abs=1e-9)
    assert r.open_positions()["mark_price"][0] == pytest.approx(104.0)


# ── Walk-forward: each window's stats value its own open position ─────


def test_walk_forward_fold_stats_value_the_open_position():
    reg = lrvx.SymbolRegistry()
    sym = reg.add_symbol("backtest", "BTCUSDT", tick_size=0.01)
    wfr = lrvx.WalkForwardRunner(reg, fee_rate=FEE, initial_capital=CAPITAL,
                                 mode="sliding", train_size=3, test_size=3, step=3)
    # Every window buys on its first bar and never sells.
    wfr.set_strategy_factory(lambda _fold: _BarOrders([sym], {0: 1.0}))
    start = np.array([T0 + i * MIN for i in range(6)], dtype=np.int64)
    opens = np.array([100.0, 101.0, 102.0, 110.0, 111.0, 112.0])
    closes = np.array([100.0, 101.0, 102.0, 110.0, 113.0, 108.0])
    folds = wfr.run_bars(start, start + MIN, opens, np.maximum(opens, closes) + 1.0,
                         np.minimum(opens, closes) - 1.0, closes, np.full(6, 10.0),
                         symbol="BTCUSDT")
    assert len(folds) == 1
    # Train [0, 3): filled at bar 1's open (101), marked at bar 2's close (102).
    # Test [3, 6): filled at bar 4's open (111), marked at bar 5's close (108).
    for key, expected in (("train_stats", 1.0), ("test_stats", -3.0)):
        stats = folds[0][key]
        assert stats["unrealized_pnl"] == pytest.approx(expected, abs=1e-9), key
        assert stats["final_equity"] == pytest.approx(
            CAPITAL + stats["net_pnl"] + expected, abs=1e-9), key
        _equity_identity(stats)
