"""``DataWriter.write_book(update_type=...)`` and ``write_books`` byte spaces.

``DataReader.read_book_updates`` returns the record byte in ``event_type``
(0 snapshot, 1 delta, 2 top). ``write_book``'s ``update_type`` and
``write_books``'s ``event_type`` take the same byte, so a read and a write
keep every row's type.

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_write_book_types.py
"""
from __future__ import annotations

import json
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
from lrvx import cli  # noqa: E402

_LEVEL = np.dtype([("price_raw", np.int64), ("qty_raw", np.int64), ("side", np.uint8)])
_BASE = 1_700_000_000_000_000_000


def _level(price: float, qty: float = 1.0):
    return np.array([(int(round(price * 1e8)), int(round(qty * 1e8)), 0)], dtype=_LEVEL)


def _writer(path: Path) -> lrvx.DataWriter:
    path.mkdir(parents=True, exist_ok=True)
    return lrvx.DataWriter(str(path), max_segment_mb=4, exchange_id=0, compression="none")


def _types(path: Path) -> list[int]:
    headers, _ = lrvx.DataReader(str(path)).read_book_updates()
    return [int(t) for t in headers["event_type"]]


class WriteBookUpdateType(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="lrvx-wbt-")
        self.root = Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def _write_mixed(self, path: Path) -> None:
        w = _writer(path)
        try:
            for i, kind in enumerate((0, 2, 2, 1)):
                ts = _BASE + i
                self.assertTrue(w.write_book(exchange_ts_ns=ts, recv_ts_ns=ts, seq=i, symbol_id=7,
                                             bids=_level(100.0 - i), asks=_level(101.0 + i, 2.0),
                                             update_type=kind))
        finally:
            w.close()

    def test_update_type_writes_each_kind(self):
        self._write_mixed(self.root / "t")
        self.assertEqual(_types(self.root / "t"), [0, 2, 2, 1])

    def test_is_snapshot_still_works(self):
        w = _writer(self.root / "t")
        try:
            w.write_book(_BASE, _BASE, 0, 7, True, _level(99.0), _level(101.0))
            w.write_book(_BASE + 1, _BASE + 1, 1, 7, False, _level(99.5), _level(100.5))
        finally:
            w.close()
        self.assertEqual(_types(self.root / "t"), [0, 1])

    def test_bad_update_type_raises(self):
        w = _writer(self.root / "t")
        try:
            with self.assertRaises(ValueError):
                w.write_book(_BASE, _BASE, 0, 7, bids=_level(99.0), asks=None, update_type=3)
            with self.assertRaises(ValueError):
                w.write_book(_BASE, _BASE, 0, 7, is_snapshot=True, bids=_level(99.0),
                             asks=None, update_type=2)
        finally:
            w.close()

    def test_write_books_round_trips_the_record_byte(self):
        self._write_mixed(self.root / "src")
        headers, levels = lrvx.DataReader(str(self.root / "src")).read_book_updates()
        w = _writer(self.root / "dst")
        try:
            self.assertEqual(w.write_books(headers, levels), 4)
        finally:
            w.close()
        copied, copied_levels = lrvx.DataReader(str(self.root / "dst")).read_book_updates()
        self.assertEqual([int(t) for t in copied["event_type"]], [0, 2, 2, 1])
        np.testing.assert_array_equal(copied_levels["price_raw"], levels["price_raw"])
        np.testing.assert_array_equal(copied_levels["qty_raw"], levels["qty_raw"])

    def test_write_books_rejects_a_frame_type_and_writes_nothing(self):
        self._write_mixed(self.root / "src")
        headers, levels = lrvx.DataReader(str(self.root / "src")).read_book_updates()
        headers = headers.copy()
        headers["event_type"][-1] = 3
        w = _writer(self.root / "dst")
        try:
            with self.assertRaises(ValueError):
                w.write_books(headers, levels)
        finally:
            w.close()
        self.assertEqual(_types(self.root / "dst"), [])


class MergedViewKeepsTops(unittest.TestCase):
    def test_materialized_merge_writes_tops(self):
        with tempfile.TemporaryDirectory(prefix="lrvx-wbt-") as d:
            src = Path(d) / "src"
            registry = lrvx.SymbolRegistry()
            sym = registry.add_symbol("hyperliquid", "BTC", tick_size=0.01)
            hook = lrvx.BinaryLogRecorderHook(str(src), max_segment_mb=4, exchange_id=0,
                                              compression="none", exchange_name="hyperliquid",
                                              instrument_type="perpetual")
            hook.add_symbol(sym, "BTC", "", "", 2, 6)
            runner = lrvx.Runner(registry, on_signal=lambda _s: None)
            runner.set_market_data_recorder(hook)
            runner.start()
            runner.on_book_snapshot(sym, [99.0], [1.0], [101.0], [1.0], _BASE)
            runner.on_book_top(sym, (99.5, 2.0), (100.5, 3.0), _BASE + 1)
            runner.stop()
            hook.close()

            out = Path(d) / "merged"
            cli._materialize_merged_tape([src], out)
            self.assertEqual(_types(out), [0, 2])
            meta = json.loads((out / "metadata.json").read_text())
            self.assertTrue(meta["has_book_snapshots"])
            self.assertFalse(meta["has_book_deltas"])
            self.assertTrue(meta["has_book_tops"])


if __name__ == "__main__":
    unittest.main()
