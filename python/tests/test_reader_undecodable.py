"""``lrvx.DataReader.stats()["segments_truncated"]``.

A frame or block the reader cannot decode ends its segment: the events before
it are delivered, the read goes on with the next segment, and the segment is
counted in ``segments_truncated``. Here an uncompressed frame is given a length
above the 10 MiB limit.

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_reader_undecodable.py
"""
from __future__ import annotations

import struct
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
_SEGMENT_HEADER = 64
_TRADE_FRAME = 12 + 48  # FrameHeader + TradeRecord


def _segment(tape_dir: Path, first_ns: int, n: int) -> None:
    w = lrvx.DataWriter(str(tape_dir), max_segment_mb=64)
    try:
        for i in range(n):
            ts = first_ns + i * _MS
            w.write_trade(exchange_ts_ns=ts, recv_ts_ns=ts, price=100.0 + i, qty=1.0,
                          trade_id=i, symbol_id=1, side=0)
    finally:
        w.close()


class UndecodableDataTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self._tmp.name)
        _segment(self.dir, _BASE, 100)
        _segment(self.dir, _BASE + 1000 * _MS, 100)
        _segment(self.dir, _BASE + 2000 * _MS, 100)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _damage_frame(self, segment: Path, frame: int) -> None:
        with open(segment, "r+b") as f:
            f.seek(_SEGMENT_HEADER + frame * _TRADE_FRAME)
            f.write(struct.pack("<I", 11 * 1024 * 1024))

    def test_clean_tape_counts_nothing(self) -> None:
        r = lrvx.DataReader(str(self.dir))
        self.assertEqual(len(r.read_trades()), 300)
        self.assertEqual(r.stats()["segments_truncated"], 0)

    def test_oversized_frame_ends_its_segment(self) -> None:
        segments = sorted(self.dir.glob("*.lrvx"))
        self.assertEqual(len(segments), 3)
        self._damage_frame(segments[1], 40)
        r = lrvx.DataReader(str(self.dir))
        trades = r.read_trades()
        self.assertEqual(len(trades), 240)
        stats = r.stats()
        self.assertEqual(stats["segments_truncated"], 1)
        self.assertEqual(stats["crc_errors"], 0)
        # The third segment is read in full after the damaged one.
        self.assertEqual(int(trades["exchange_ts_ns"][-1]), _BASE + 2099 * _MS)


if __name__ == "__main__":
    unittest.main()
