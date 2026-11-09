"""``EventTypeStatsAggregator`` rows through the Python binding.

The structured dtype mirrors the C++ row field for field. A field missing
from the dtype registration would shift every column after it, so the test
pins a whole row with a distinct count per column, not just one column.

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_event_type_stats.py
"""
from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
for cand in ("build/python", "build-py312/python"):
    p = REPO_ROOT / cand
    if p.is_dir():
        sys.path.insert(0, str(p))
        break

import numpy as np  # noqa: E402

import lrvx  # noqa: E402

_LEVEL = np.dtype([("price_raw", np.int64), ("qty_raw", np.int64), ("side", np.uint8)])
_BASE = 1_700_000_000_000_000_000


def _level(price: float):
    return np.array([(int(round(price * 1e8)), 100_000_000, 0)], dtype=_LEVEL)


class EventTypeStatsRow(unittest.TestCase):
    def test_row_lines_up_with_the_cpp_struct(self):
        tmp = tempfile.TemporaryDirectory(prefix="lrvx-ets-")
        self.addCleanup(tmp.cleanup)
        tape = Path(tmp.name) / "tape"
        tape.mkdir()
        w = lrvx.DataWriter(str(tape), max_segment_mb=4, exchange_id=0, compression="none")
        try:
            ts = _BASE
            for i in range(3):
                ts += 1
                w.write_trade(exchange_ts_ns=ts, recv_ts_ns=ts, price=100.0, qty=1.0,
                              trade_id=i, symbol_id=7, side=0)
            for i in range(2):
                ts += 1
                w.write_book(exchange_ts_ns=ts, recv_ts_ns=ts, seq=i, symbol_id=7,
                             is_snapshot=True, bids=_level(99.0), asks=_level(101.0))
            for i in range(5):
                ts += 1
                w.write_book(exchange_ts_ns=ts, recv_ts_ns=ts, seq=10 + i, symbol_id=7,
                             is_snapshot=False, bids=_level(99.5), asks=_level(100.5))
            for i in range(4):
                ts += 1
                w.write_book(exchange_ts_ns=ts, recv_ts_ns=ts, seq=20 + i, symbol_id=7,
                             bids=_level(99.7), asks=_level(100.3), update_type=2)
        finally:
            w.close()

        agg = lrvx.EventTypeStatsAggregator()
        self.assertTrue(lrvx.DataReader(str(tape)).run([agg], n_threads=1))
        rows = agg.result()
        self.assertEqual(rows.dtype.names,
                         ("symbol_id", "trades", "book_snapshots", "book_deltas", "book_tops"))
        self.assertEqual(len(rows), 1)
        self.assertEqual(tuple(int(v) for v in rows[0]), (7, 3, 2, 5, 4))


if __name__ == "__main__":
    unittest.main()
