"""``lrvx.DataReader``'s two segment switches and the segment counters.

``order_segments_by_time`` (default ``True``) reads segments in time order of
their first event and merges Sorted segments whose ranges overlap;
``False`` reads them in file-name order. ``skip_segments_by_time`` (default
``True``) leaves out, with ``from_ns`` or ``to_ns`` set, the segments whose
proven bounds lie outside the range; ``False`` decodes every segment and
delivers the same events. ``stats()`` reports ``segments_skipped``,
``segments_reordered`` and ``segments_interleaved``.

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_reader_segment_switches.py
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

import lrvx  # noqa: E402

_MS = 1_000_000
_BASE = 1_700_000_000 * 1_000_000_000


def _segment(tape_dir: Path, first_ns: int, n: int, step_ns: int) -> None:
    # One writer per segment: files are named by the time the writer opened
    # them, so they list in the order written.
    w = lrvx.DataWriter(str(tape_dir), max_segment_mb=64)
    try:
        for i in range(n):
            ts = first_ns + i * step_ns
            w.write_trade(exchange_ts_ns=ts, recv_ts_ns=ts, price=100.0 + i, qty=1.0,
                          trade_id=i, symbol_id=1, side=0)
    finally:
        w.close()


class SegmentSwitchesTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self._tmp.name)
        # First file: even milliseconds from 2. Second: odd ones from 1, so it
        # starts first in time and overlaps the first. Third: after both.
        _segment(self.dir, _BASE + 2 * _MS, 100, 2 * _MS)
        _segment(self.dir, _BASE + _MS, 50, 2 * _MS)
        _segment(self.dir, _BASE + 1000 * _MS, 50, _MS)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_time_order_is_the_default(self) -> None:
        r = lrvx.DataReader(str(self.dir))
        ts = r.read_trades()["exchange_ts_ns"].tolist()
        self.assertEqual(len(ts), 200)
        self.assertEqual(ts, sorted(ts))
        s = r.stats()
        self.assertEqual(s["segments_reordered"], 2)
        self.assertEqual(s["segments_interleaved"], 2)
        self.assertEqual(s["segments_skipped"], 0)

    def test_file_name_order_on_request(self) -> None:
        r = lrvx.DataReader(str(self.dir), order_segments_by_time=False)
        ts = r.read_trades()["exchange_ts_ns"].tolist()
        self.assertEqual(len(ts), 200)
        self.assertEqual(ts[0], _BASE + 2 * _MS)
        self.assertNotEqual(ts, sorted(ts))
        s = r.stats()
        self.assertEqual(s["segments_reordered"], 0)
        self.assertEqual(s["segments_interleaved"], 0)

    def test_skip_switch_changes_the_work_not_the_result(self) -> None:
        skip = lrvx.DataReader(str(self.dir), from_ns=_BASE + 500 * _MS)
        walk = lrvx.DataReader(str(self.dir), from_ns=_BASE + 500 * _MS,
                               skip_segments_by_time=False)
        a = skip.read_trades()
        b = walk.read_trades()
        self.assertEqual(len(a), 50)
        self.assertEqual(a.tobytes(), b.tobytes())
        self.assertEqual(skip.stats()["segments_skipped"], 2)
        self.assertEqual(walk.stats()["segments_skipped"], 0)
        self.assertEqual(walk.stats()["files_read"], 3)


if __name__ == "__main__":
    unittest.main()
