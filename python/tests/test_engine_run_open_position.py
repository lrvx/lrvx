"""``Engine.run``: the position still open when the signals run out, valued.

``Engine.run`` replays every loaded symbol's bars merged in time order and
fills the signals against them. A position still open at the end is valued
at the last close the run replayed for its symbol -- the latest bar in time,
not the last row loaded -- and the stats carry it:

    unrealized_pnl  sum over symbols of (mark - entry) * qty for a long,
                    (entry - mark) * qty for a short, no exit fee
    final_equity    initial_capital + net_pnl + unrealized_pnl

``net_pnl``, ``total_pnl``, ``final_capital`` and ``return_pct`` keep their
meaning (closed trades only). Valued at its last fill instead, the open
unit below would read 4 rather than 10.

Mirrors the ``Engine.run`` block of
node/test/test_backtest_open_position_at_end.js.

Run from repo root:
    PYTHONPATH=build/python python3 -m pytest python/tests/test_engine_run_open_position.py
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
for cand in ("build/python", "build-py/python", "build-py312/python"):
    p = REPO_ROOT / cand
    if p.is_dir():
        sys.path.insert(0, str(p))
        break

import numpy as np  # noqa: E402

import lrvx  # noqa: E402

CAPITAL = 10_000.0
T0 = 1_767_225_600  # seconds; Engine normalises to nanoseconds
CLOSES = [100.0, 102.0, 104.0, 107.0, 110.0]


def _ohlcv(ts_sec, closes):
    # open, high, low and close all differ, so a mark taken from any price
    # but the close shows.
    closes = np.asarray(closes, dtype=np.float64)
    return {
        "ts": np.asarray(ts_sec, dtype=np.int64),
        "open": closes - 0.5,
        "high": closes + 1.0,
        "low": closes - 1.0,
        "close": closes,
        "volume": np.full(len(closes), 10.0),
    }


def _single(fee_rate=0.0):
    eng = lrvx.Engine(CAPITAL, fee_rate)
    ts = [T0 + 60 * i for i in range(len(CLOSES))]
    eng.load_ohlcv(_ohlcv(ts, CLOSES), "BTCUSDT")
    return eng, ts


class EngineRunOpenPosition(unittest.TestCase):

    def assert_equity_identity(self, s, label):
        self.assertAlmostEqual(
            s.final_equity, s.initial_capital + s.net_pnl + s.unrealized_pnl, places=9,
            msg=f"{label}: final_equity = initial_capital + net_pnl + unrealized_pnl")

    def test_long_open_at_end_is_marked_at_the_last_close(self):
        # Buys 2 on bar 0 (close 100), sells 1 on bar 2 (close 104), holds
        # the other unit to the end; the last close is 110.
        eng, ts = _single()
        sig = lrvx.SignalBuilder()
        sig.buy(ts[0], 2.0)
        sig.sell(ts[2], 1.0)
        s = eng.run(sig)
        self.assertEqual(s.total_trades, 1)
        self.assertAlmostEqual(s.total_pnl, 4.0, places=9, msg="closed unit, 104 - 100")
        self.assertAlmostEqual(s.unrealized_pnl, 10.0, places=9,
                               msg="open unit marked at the last close, 110 - 100 "
                                   "(4.0 means it was valued at its last fill)")
        self.assertAlmostEqual(s.final_equity, CAPITAL + s.net_pnl + 10.0, places=9)
        self.assert_equity_identity(s, "long")
        self.assertAlmostEqual(s.final_capital, CAPITAL + s.net_pnl, places=9,
                               msg="final_capital leaves the open unit out")

    def test_short_open_at_end_is_marked_at_the_last_close(self):
        # Sells 2 on bar 0 (100), buys 1 back on bar 2 (104), stays short 1.
        eng, ts = _single()
        sig = lrvx.SignalBuilder()
        sig.sell(ts[0], 2.0)
        sig.buy(ts[2], 1.0)
        s = eng.run(sig)
        self.assertEqual(s.total_trades, 1)
        self.assertAlmostEqual(s.total_pnl, -4.0, places=9)
        self.assertAlmostEqual(s.unrealized_pnl, -10.0, places=9,
                               msg="short marked at the last close, 100 - 110")
        self.assert_equity_identity(s, "short")

    def test_fees_stay_out_of_the_mark(self):
        eng, ts = _single(fee_rate=0.001)
        sig = lrvx.SignalBuilder()
        sig.buy(ts[0], 2.0)
        sig.sell(ts[2], 1.0)
        s = eng.run(sig)
        self.assertAlmostEqual(s.unrealized_pnl, 10.0, places=9,
                               msg="no exit fee is taken from the open unit")
        self.assertGreater(s.total_fees, 0.0)
        self.assert_equity_identity(s, "with fees")

    def test_flat_run_has_nothing_to_value(self):
        eng, ts = _single()
        sig = lrvx.SignalBuilder()
        sig.buy(ts[0], 1.0)
        sig.sell(ts[3], 1.0)
        s = eng.run(sig)
        self.assertEqual(s.total_trades, 1)
        self.assertEqual(s.unrealized_pnl, 0.0)
        self.assertAlmostEqual(s.final_equity, s.final_capital, places=9)

    def test_each_symbol_is_marked_at_its_own_last_close(self):
        # Two symbols whose bars interleave in time: A on the minute, B on
        # the half minute, B's last bar after A's. Long 1 A @ 100, short 2
        # B @ 50. A's last close is 103 (+3), B's 54 (-8). Marking every
        # symbol at the last bar replayed (B's 54) would give A -46.
        eng = lrvx.Engine(CAPITAL, 0.0)
        a_ts = [T0 + 60 * i for i in range(4)]
        b_ts = [T0 + 30 + 60 * i for i in range(5)]
        eng.load_ohlcv(_ohlcv(a_ts, [100.0, 101.0, 102.0, 103.0]), "A")
        eng.load_ohlcv(_ohlcv(b_ts, [50.0, 51.0, 52.0, 53.0, 54.0]), "B")
        sig = lrvx.SignalBuilder()
        sig.buy(a_ts[0], 1.0, "A")
        sig.sell(b_ts[0], 2.0, "B")
        s = eng.run(sig)
        self.assertEqual(s.total_trades, 0)
        self.assertAlmostEqual(s.unrealized_pnl, 3.0 - 8.0, places=9,
                               msg="A at 103, B at 54, each at its own last close")
        self.assert_equity_identity(s, "two symbols")

    def test_mark_is_the_latest_bar_in_time_not_the_last_row_loaded(self):
        # A's rows arrive out of time order: the last row (ts +120, close
        # 102) is not its latest bar (ts +180, close 103). The run replays
        # in time order, so the last close it replayed for A is 103. B's
        # bars interleave with A's and end later.
        eng = lrvx.Engine(CAPITAL, 0.0)
        a_ts = [T0, T0 + 180, T0 + 60, T0 + 120]
        a_close = [100.0, 103.0, 101.0, 102.0]
        b_ts = [T0 + 30 + 60 * i for i in range(5)]
        eng.load_ohlcv(_ohlcv(a_ts, a_close), "A")
        eng.load_ohlcv(_ohlcv(b_ts, [50.0, 51.0, 52.0, 53.0, 54.0]), "B")
        sig = lrvx.SignalBuilder()
        sig.buy(T0, 1.0, "A")
        sig.buy(b_ts[0], 1.0, "B")
        s = eng.run(sig)
        self.assertAlmostEqual(s.unrealized_pnl, 3.0 + 4.0, places=9,
                               msg="A marked at 103 (its latest bar), not 102 (its last row); "
                                   "B at 54")
        self.assert_equity_identity(s, "out-of-order rows")

    def test_to_dict_carries_both(self):
        eng, ts = _single()
        sig = lrvx.SignalBuilder()
        sig.buy(ts[0], 2.0)
        sig.sell(ts[2], 1.0)
        s = eng.run(sig)
        d = s.to_dict()
        self.assertAlmostEqual(d["unrealized_pnl"], 10.0, places=9)
        self.assertAlmostEqual(d["final_equity"], s.final_equity, places=9)


if __name__ == "__main__":
    unittest.main()
