"""Structured arrays the readers and tape aggregators return have their
padding bytes zeroed.

The record dtypes leave gaps between fields (the `_pad` bytes of the C
structs are not dtype fields). MergedTapeReader.read_books filled every field
and left the gaps as whatever the allocator handed back, so hashing a whole
buffer (`arr.tobytes()`) gave a different answer on two reads of one tape.
The aggregator rows have implicit padding, copied from the C++ rows as is.
Every read here is checked three ways: the gap bytes are zero, two reads
hash the same with the heap dirtied in between, and the gap bytes are still
zero in a child process whose allocator fills every new block with a
non-zero byte (MALLOC_PERTURB_ for glibc, MallocPreScribble for macOS), so a
byte the binding leaves unset cannot read as zero by luck.
"""

from __future__ import annotations

import hashlib
import os
import subprocess
import sys
import tempfile
import unittest

import numpy as np

import lrvx

BASE = 1_700_000_000_000_000_000
MS = 1_000_000


def _gap_mask(dtype: np.dtype) -> np.ndarray:
    """True for the bytes of one item that no field covers."""
    covered = np.zeros(dtype.itemsize, dtype=bool)
    for name in dtype.names:
        field_dtype, offset = dtype.fields[name][:2]
        covered[offset:offset + field_dtype.itemsize] = True
    return ~covered


def _dirty_heap(nbytes: int) -> None:
    """Hand freed, non-zero blocks of the sizes the next read asks for back
    to the allocator, so a read that leaves bytes unset shows garbage."""
    junk = []
    for size in (nbytes, max(nbytes // 2, 64), 4096, 65536):
        for _ in range(8):
            junk.append(np.full(size, 0xA5, dtype=np.uint8))
    del junk


def _write_tape(path: str) -> None:
    """Trades and book snapshots on one symbol, with the metadata.json that
    MergedTapeReader needs."""
    registry = lrvx.SymbolRegistry()
    sym = registry.add_symbol("bybit", "BTCUSDT", tick_size=0.01)
    hook = lrvx.BinaryLogRecorderHook(
        path, max_segment_mb=4, exchange_id=0, compression="none",
        exchange_name="bybit", instrument_type="perpetual",
    )
    hook.add_symbol(sym, "BTCUSDT", "", "", 2, 6)
    runner = lrvx.Runner(registry, on_signal=lambda _s: None)
    runner.set_market_data_recorder(hook)
    runner.start()
    for i in range(400):
        ts = BASE + i * MS
        runner.on_trade(sym, price=100.0 + i % 7, qty=0.5 + i % 3,
                        is_buy=i % 2 == 0, ts_ns=ts)
        if i % 4 == 0:
            runner.on_book_snapshot(sym, [99.0 - i % 5, 98.0], [1.0, 2.0],
                                    [101.0 + i % 5], [1.5], ts + 2)
    runner.stop()
    hook.close()


def _reads_of(tape: str) -> dict:
    def aggregate(agg):
        lrvx.DataReader(tape).run([agg], n_threads=1)
        return agg.result()

    return {
        "DataReader.read_trades": lambda: lrvx.DataReader(tape).read_trades(),
        "DataReader.read_trades_from": lambda: lrvx.DataReader(tape).read_trades_from(BASE),
        "DataReader.read_bbo": lambda: lrvx.DataReader(tape).read_bbo(),
        "DataReader.read_bbo_from": lambda: lrvx.DataReader(tape).read_bbo_from(BASE),
        "DataReader.read_book_updates": lambda: lrvx.DataReader(tape).read_book_updates(),
        "DataReader.read_book_updates_from":
            lambda: lrvx.DataReader(tape).read_book_updates_from(BASE),
        "DataReader.read_option_quotes_from":
            lambda: lrvx.DataReader(tape).read_option_quotes_from(BASE),
        "MergedTapeReader.read_trades": lambda: lrvx.MergedTapeReader([tape]).read_trades(),
        "MergedTapeReader.read_books": lambda: lrvx.MergedTapeReader([tape]).read_books(),
        "EventTypeStatsAggregator.result":
            lambda: aggregate(lrvx.EventTypeStatsAggregator()),
        "BinCountAggregator.result":
            lambda: aggregate(lrvx.BinCountAggregator(10 * MS, True, True)),
        "VolumeBinAggregator.result":
            lambda: aggregate(lrvx.VolumeBinAggregator(10 * MS, True, True)),
        "OHLCBinAggregator.result":
            lambda: aggregate(lrvx.OHLCBinAggregator(10 * MS, True)),
        "BookSnapshotBinAggregator.result":
            lambda: aggregate(lrvx.BookSnapshotBinAggregator(10 * MS, 2)),
    }


def _arrays(result):
    return list(result) if isinstance(result, tuple) else [result]


def _nonzero_padding(tape: str) -> list:
    """Names of the reads whose result has a non-zero gap byte."""
    bad = []
    for name, read in _reads_of(tape).items():
        for k, arr in enumerate(_arrays(read())):
            mask = _gap_mask(arr.dtype)
            if not mask.any() or arr.size == 0:
                continue
            raw = np.frombuffer(arr.tobytes(), dtype=np.uint8).reshape(
                arr.size, arr.dtype.itemsize)
            if raw[:, mask].any():
                bad.append(f"{name}[{k}]")
    return bad


class StructuredArrayPaddingTests(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory()
        cls.tape = os.path.join(cls._tmp.name, "tape")
        _write_tape(cls.tape)

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def _reads(self):
        return _reads_of(self.tape)

    @staticmethod
    def _arrays(result):
        return _arrays(result)

    def test_padding_bytes_are_zero(self):
        for name, read in self._reads().items():
            with self.subTest(read=name):
                for arr in self._arrays(read()):
                    _dirty_heap(arr.nbytes)
                for k, arr in enumerate(self._arrays(read())):
                    mask = _gap_mask(arr.dtype)
                    if not mask.any() or arr.size == 0:
                        continue
                    raw = np.frombuffer(arr.tobytes(), dtype=np.uint8).reshape(
                        arr.size, arr.dtype.itemsize)
                    self.assertFalse(raw[:, mask].any(),
                                     f"{name}[{k}] has non-zero padding")

    def test_padding_is_zero_when_the_allocator_scribbles(self):
        env = dict(os.environ, MALLOC_PERTURB_="165", MallocPreScribble="1")
        r = subprocess.run([sys.executable, os.path.abspath(__file__), "--child", self.tape],
                           env=env, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_two_reads_hash_the_same(self):
        for name, read in self._reads().items():
            with self.subTest(read=name):
                first = [hashlib.sha256(a.tobytes()).hexdigest()
                         for a in self._arrays(read())]
                for arr in self._arrays(read()):
                    _dirty_heap(arr.nbytes)
                second = [hashlib.sha256(a.tobytes()).hexdigest()
                          for a in self._arrays(read())]
                self.assertEqual(first, second)

    def test_the_tape_has_rows_for_every_read(self):
        reads = self._reads()
        self.assertEqual(reads["DataReader.read_trades"]().size, 400)
        self.assertEqual(reads["MergedTapeReader.read_books"]()[0].size, 100)
        self.assertEqual(reads["DataReader.read_bbo"]().size, 100)


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--child":
        bad = _nonzero_padding(sys.argv[2])
        if bad:
            print("non-zero padding: " + ", ".join(bad))
            sys.exit(1)
        sys.exit(0)
    unittest.main()
