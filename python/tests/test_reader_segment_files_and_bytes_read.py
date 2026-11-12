"""``lrvx.DataReader`` reports its segments and the bytes it read.

``segment_files()`` used to return an empty list until something else had
scanned the directory, while ``segments()`` on the same fresh reader listed
every segment.

``stats()["bytes_read"]`` is the number of segment-file bytes a read consumed
as event data: every frame of an uncompressed segment, every block of a
compressed one, without the segment header or the index. It used to stay 0
after any read. A full read counts, per segment, the bytes between the header
and the index, whichever read path delivered it.

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_reader_segment_files_and_bytes_read.py
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

_SEC = 1_000_000_000
_BASE = 1_700_000_000 * _SEC
_SEGMENTS = 5
_PER_SEGMENT = 300

_HEADER_SIZE = 64
_HAS_INDEX = 0x01


def _write_tape(tape_dir: Path, compression: str) -> None:
    tape_dir.mkdir(parents=True, exist_ok=True)
    trade_id = 1
    for k in range(_SEGMENTS):
        w = lrvx.DataWriter(str(tape_dir), max_segment_mb=64, exchange_id=0,
                            compression=compression)
        try:
            for i in range(_PER_SEGMENT):
                ts = _BASE + (k * 1000 + i) * _SEC
                w.write_trade(exchange_ts_ns=ts, recv_ts_ns=ts, price=100.0 + i * 0.01,
                              qty=1.0 + (i % 7), trade_id=trade_id, symbol_id=1 + i % 2,
                              side=i % 2)
                trade_id += 1
        finally:
            w.close()


def _data_bytes(path: str) -> int:
    """Bytes between the segment header and the index (or the end of file)."""
    raw = Path(path).read_bytes()
    flags = raw[6]
    (index_offset,) = struct.unpack_from("<Q", raw, 40)
    end = index_offset if (flags & _HAS_INDEX) and index_offset > 0 else len(raw)
    return end - _HEADER_SIZE


class SegmentFilesAndBytesReadTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="reader-stats-")
        self.root = Path(self._tmp.name)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _tape(self, compression: str) -> Path:
        tape = self.root / compression
        _write_tape(tape, compression)
        return tape

    def test_segment_files_on_a_fresh_reader(self) -> None:
        tape = self._tape("none")
        on_disk = sorted(str(p) for p in tape.glob("*.lrvx"))
        self.assertEqual(len(on_disk), _SEGMENTS)

        files = lrvx.DataReader(str(tape)).segment_files()
        self.assertEqual(sorted(files), on_disk)

        reader = lrvx.DataReader(str(tape))
        self.assertEqual(reader.segment_files(),
                         [s["path"] for s in reader.segments()])

    def test_bytes_read_on_every_read_path(self) -> None:
        for compression in ("none", "lz4"):
            tape = self._tape(compression)
            expected = sum(_data_bytes(s["path"]) for s in lrvx.DataReader(str(tape)).segments())
            self.assertGreater(expected, 0)

            reads = {
                "read_trades": lambda r: r.read_trades(),
                "read_trades_from": lambda r: r.read_trades_from(_BASE),
                "run(1)": lambda r: r.run([lrvx.EventTypeStatsAggregator()], n_threads=1),
                "run(4)": lambda r: r.run([lrvx.EventTypeStatsAggregator()], n_threads=4),
            }
            for name, read in reads.items():
                reader = lrvx.DataReader(str(tape))
                read(reader)
                stats = reader.stats()
                self.assertEqual(stats["events_read"], _SEGMENTS * _PER_SEGMENT,
                                 f"{compression} {name}")
                self.assertEqual(stats["bytes_read"], expected, f"{compression} {name}")

    def test_a_skipped_segment_adds_no_bytes(self) -> None:
        tape = self._tape("none")
        files = [s["path"] for s in lrvx.DataReader(str(tape)).segments()]
        # Exactly the third segment; the others are proven outside the range.
        reader = lrvx.DataReader(str(tape), from_ns=_BASE + 2000 * _SEC,
                                 to_ns=_BASE + (2000 + _PER_SEGMENT - 1) * _SEC)
        trades = reader.read_trades()
        self.assertEqual(trades.size, _PER_SEGMENT)
        stats = reader.stats()
        self.assertEqual(stats["files_read"], 1)
        self.assertEqual(stats["segments_skipped"], _SEGMENTS - 1)
        self.assertEqual(stats["bytes_read"], _data_bytes(files[2]))


if __name__ == "__main__":
    unittest.main()
