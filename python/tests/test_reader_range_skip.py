"""Time-ranged ``lrvx.DataReader`` reads skip segments outside the range.

A reader built with ``from_ns`` / ``to_ns`` must return exactly the trades a
full read filtered to the same inclusive range returns, and must not decode
segments whose recorded bounds put them outside the range.
``stats()['segments_skipped']`` counts those; ``stats()['files_read']`` counts
the segments that were decoded.

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_reader_range_skip.py
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

_SEC = 1_000_000_000
_BASE = 1_700_000_000 * _SEC
_SEGMENTS = 6
_PER_SEGMENT = 40


def _write_tape(tape_dir: Path, compression: str) -> None:
    """Six segments, one writer each; segment k holds 40 trades one second
    apart starting at k * 100 s, alternating symbols 1 and 2."""
    tape_dir.mkdir(parents=True, exist_ok=True)
    trade_id = 1
    for k in range(_SEGMENTS):
        w = lrvx.DataWriter(str(tape_dir), max_segment_mb=64, exchange_id=0,
                            compression=compression)
        try:
            for i in range(_PER_SEGMENT):
                ts = _BASE + (k * 100 + i) * _SEC
                w.write_trade(exchange_ts_ns=ts, recv_ts_ns=ts, price=100.0, qty=1.0,
                              trade_id=trade_id, symbol_id=1 + i % 2, side=0)
                trade_id += 1
        finally:
            w.close()


class RangeSkipTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="range-skip-")
        self.root = Path(self._tmp.name)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _check(self, compression: str) -> None:
        tape = self.root / compression
        _write_tape(tape, compression)
        self.assertEqual(len(list(tape.glob("*.lrvx"))), _SEGMENTS)

        full = lrvx.DataReader(str(tape)).read_trades()
        self.assertEqual(full.size, _SEGMENTS * _PER_SEGMENT)

        for lo, hi, symbols in [
            (310, 320, None),
            (300, 339, None),  # exactly segment 3, both ends inclusive
            (150, 180, None),  # a gap
            (139, 400, [2]),
            (-5, 700, None),
        ]:
            from_ns, to_ns = _BASE + lo * _SEC, _BASE + hi * _SEC
            reader = lrvx.DataReader(str(tape), from_ns=from_ns, to_ns=to_ns,
                                     symbols=symbols)
            got = reader.read_trades()
            ts = full["exchange_ts_ns"]
            mask = (ts >= from_ns) & (ts <= to_ns)
            if symbols is not None:
                mask &= np.isin(full["symbol_id"], symbols)
            np.testing.assert_array_equal(got, full[mask],
                                          err_msg=f"{compression} [{lo}, {hi}]")

        # One segment's worth: the others are not decoded.
        reader = lrvx.DataReader(str(tape), from_ns=_BASE + 310 * _SEC,
                                 to_ns=_BASE + 320 * _SEC)
        self.assertEqual(reader.read_trades().size, 11)
        stats = reader.stats()
        if compression == "none":
            self.assertEqual(stats["files_read"], 1)
            self.assertEqual(stats["segments_skipped"], _SEGMENTS - 1)
        else:
            # Later segments are excluded by their block index without a
            # decode; earlier ones cost their last block.
            self.assertEqual(stats["segments_skipped"], 2)
            self.assertEqual(stats["files_read"], 4)

    def test_uncompressed(self) -> None:
        self._check("none")

    def test_lz4(self) -> None:
        self._check("lz4")


if __name__ == "__main__":
    unittest.main()
