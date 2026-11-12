"""``DataReader.run()`` with its default arguments delivers a tape whose Sorted
segments overlap in time order.

``run()`` defaults to ``n_threads=0`` (auto), the multi-threaded path. That
path splits each segment across workers and used to take segments one after
the other, so on a tape with two Sorted segments covering the same window
(two captures of one window, an import that overlaps a recording) the
aggregators saw all of the first segment and then went back in time for the
second, without a warning. The single-threaded path merges such segments
event by event. Now the multi-threaded path delivers runs of overlapping
segments through the same ordered merge, and partitions everything else as
before.

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_reader_run_overlap_order.py
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

_MS = 1_000_000
_BASE = 1_700_000_000 * 1_000_000_000
# Enough 1000-event blocks per segment for the auto path to split it across
# workers on any machine this suite runs on.
_A = 80_000
_B = 40_000


def _write_segment(tape_dir: Path, first_ns: int, n: int, step_ns: int, price0: float) -> None:
    sub = tape_dir / f"tmp{first_ns}"
    w = lrvx.DataWriter(str(sub), max_segment_mb=4096, compression="lz4")
    try:
        ts = first_ns + np.arange(n, dtype=np.int64) * step_ns
        w.write_trades(ts, ts, price0 + np.arange(n) * 0.01, np.ones(n),
                       np.arange(n, dtype=np.uint64), np.ones(n, dtype=np.uint32),
                       (np.arange(n) % 2).astype(np.uint8))
    finally:
        w.close()
    for f in sub.iterdir():
        f.rename(tape_dir / f.name)
    sub.rmdir()


def _run(tape_dir: Path, **run_kwargs):
    reader = lrvx.DataReader(str(tape_dir))
    # One OHLC bucket over the whole tape, plus the two sliding-window
    # aggregators, which see a step back in time as the window restarting.
    aggs = [lrvx.OHLCBinAggregator(10**15),
            lrvx.PeakAggregator([200 * _MS], top_n=5),
            lrvx.QuantileAggregator([500 * _MS], [0.5, 0.9])]
    reader.run(aggs, **run_kwargs)
    return [a.result() for a in aggs], reader.stats()


class RunOverlapOrderTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self._tmp.name)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_overlapping_segments_reach_the_aggregators_in_time_order(self) -> None:
        # a: every even millisecond; b, written after it: the odd ones of the
        # first half. b ends long before a does.
        _write_segment(self.dir, _BASE, _A, 2 * _MS, 100.0)
        _write_segment(self.dir, _BASE + _MS, _B, 2 * _MS, 5000.0)

        single, _ = _run(self.dir, n_threads=1)
        default, stats = _run(self.dir)
        ohlc = single[0]
        self.assertEqual(len(ohlc), 1)
        # The latest trade is a's last; b's last came 40 s earlier.
        self.assertAlmostEqual(int(ohlc["close_raw"][0]) / 1e8, 100.0 + (_A - 1) * 0.01, places=4)
        for got, want, name in zip(default, single, ("ohlc", "peak", "quantile")):
            self.assertEqual(repr(got), repr(want), name)
        self.assertEqual(stats["events_read"], _A + _B)
        self.assertEqual(stats["segments_interleaved"], 2)

    def test_in_order_tape_reads_as_before(self) -> None:
        _write_segment(self.dir, _BASE, _A, _MS, 100.0)
        _write_segment(self.dir, _BASE + _A * _MS, _A, _MS, 900.0)
        single, _ = _run(self.dir, n_threads=1)
        default, stats = _run(self.dir)
        # The sliding-window aggregators may under-count across the seams of
        # a partitioned segment (IAggregator::merge); OHLC merges exactly.
        self.assertEqual(repr(default[0]), repr(single[0]))
        self.assertEqual(stats["segments_interleaved"], 0)


if __name__ == "__main__":
    unittest.main()
