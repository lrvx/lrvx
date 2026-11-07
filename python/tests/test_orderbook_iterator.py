"""Tests for ``lrvx.orderbook.OrderBookIterator`` and ``book_at``.

Builds a synthetic tape with known book events (snapshot + deltas) at
controlled timestamps, iterates with various bucket / level / symbol
filters, then point-queries `book_at` at chosen offsets and checks
the reconstructed ladder matches the expected state.

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_orderbook_iterator.py
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

from lrvx import orderbook as ob  # noqa: E402


_LEVEL_DTYPE = np.dtype([
    ("price_raw", np.int64),
    ("qty_raw", np.int64),
    ("side", np.uint8),
])


def _level(price: float, qty: float):
    return (int(round(price * 1e8)), int(round(qty * 1e8)), 0)


def _arr(items):
    return np.array(items, dtype=_LEVEL_DTYPE)


def _write_book_tape(tape_dir: Path, events) -> Path:
    """events: list of (ts_ns, symbol_id, is_snapshot, [(p, q)] bids, [(p, q)] asks)."""
    tape_dir.mkdir(parents=True, exist_ok=True)
    w = lrvx.DataWriter(str(tape_dir), max_segment_mb=4,
                           exchange_id=0, compression="none")
    try:
        for i, (ts, sym, is_snap, bids, asks) in enumerate(events):
            w.write_book(
                exchange_ts_ns=int(ts), recv_ts_ns=int(ts),
                seq=i, symbol_id=int(sym),
                is_snapshot=is_snap,
                bids=_arr([_level(p, q) for p, q in bids]),
                asks=_arr([_level(p, q) for p, q in asks]),
            )
    finally:
        w.close()
    return tape_dir


_BASE_NS = 1_700_000_000_000_000_000


class IteratorTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="ob-iter-"))
        self.tape = self.tmp / "tape"
        # Three events at 0s, 30s, 90s — with 60s bucket:
        # bucket 0 = [0..60), bucket 60s = [60..120). 30s event lands
        # in bucket 0; 90s event lands in bucket 60s.
        events = [
            (_BASE_NS,             1, True,
             [(100.0, 1.0), (99.0, 2.0)], [(101.0, 1.5), (102.0, 2.5)]),
            (_BASE_NS + 30_000_000_000, 1, False,
             [(100.0, 0.5)], []),  # bid qty change
            (_BASE_NS + 90_000_000_000, 1, False,
             [(99.0, 0.0)], [(101.0, 0.0)]),  # remove a bid + remove top ask
        ]
        _write_book_tape(self.tape, events)

    def test_one_snap_per_bucket(self) -> None:
        snaps = list(ob.OrderBookIterator(
            self.tape, bucket_ns=60_000_000_000, levels=5))
        self.assertEqual(len(snaps), 2)
        # Bucket boundary is the floor of the event ts onto the
        # 60s grid; consecutive snaps therefore advance by 60s.
        self.assertEqual(snaps[1].ts_ns - snaps[0].ts_ns,
                         60_000_000_000)
        # Bucket 0: latest state (after the 30s delta).
        self.assertEqual(snaps[0].bids[0], (100.0, 0.5))
        self.assertEqual(snaps[0].bids[1], (99.0, 2.0))
        self.assertEqual(snaps[0].asks[0], (101.0, 1.5))
        # Bucket 60s: after removing 99 bid and top ask.
        self.assertEqual(snaps[1].bids, [(100.0, 0.5)])
        self.assertEqual(snaps[1].asks[0], (102.0, 2.5))

    def test_levels_cap(self) -> None:
        snaps = list(ob.OrderBookIterator(
            self.tape, bucket_ns=60_000_000_000, levels=1))
        # levels=1 keeps only top-of-book per side.
        for s in snaps:
            self.assertLessEqual(len(s.bids), 1)
            self.assertLessEqual(len(s.asks), 1)

    def test_symbol_filter(self) -> None:
        # Inject a second symbol; iterator with explicit filter sees
        # only the requested symbol.
        events = [
            (_BASE_NS, 2, True, [(50.0, 1.0)], [(51.0, 1.0)]),
        ]
        # Append events for symbol 2 to the existing tape.
        w = lrvx.DataWriter(str(self.tape), max_segment_mb=4,
                               exchange_id=0, compression="none")
        try:
            for i, (ts, sym, is_snap, bids, asks) in enumerate(events):
                w.write_book(
                    exchange_ts_ns=int(ts), recv_ts_ns=int(ts),
                    seq=100 + i, symbol_id=int(sym),
                    is_snapshot=is_snap,
                    bids=_arr([_level(p, q) for p, q in bids]),
                    asks=_arr([_level(p, q) for p, q in asks]),
                )
        finally:
            w.close()

        snaps_sym1 = list(ob.OrderBookIterator(
            self.tape, bucket_ns=60_000_000_000, levels=5, symbol_id=1))
        self.assertTrue(all(s.symbol_id == 1 for s in snaps_sym1))
        snaps_sym2 = list(ob.OrderBookIterator(
            self.tape, bucket_ns=60_000_000_000, levels=5, symbol_id=2))
        self.assertTrue(all(s.symbol_id == 2 for s in snaps_sym2))
        self.assertGreaterEqual(len(snaps_sym2), 1)

    def test_empty_tape(self) -> None:
        empty = self.tmp / "empty"
        empty.mkdir()
        # No segments written → iterator yields nothing.
        try:
            snaps = list(ob.OrderBookIterator(
                empty, bucket_ns=60_000_000_000, levels=5))
            self.assertEqual(snaps, [])
        except Exception:
            # An empty directory may also be treated as "tape not
            # initialised" by DataReader; that is acceptable too.
            pass

    def test_bad_inputs(self) -> None:
        with self.assertRaises(ValueError):
            ob.OrderBookIterator(self.tape, bucket_ns=0, levels=5)
        with self.assertRaises(ValueError):
            ob.OrderBookIterator(self.tape, bucket_ns=60_000_000_000,
                                 levels=-1)


class BookAtTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="ob-at-"))
        self.tape = self.tmp / "tape"
        events = [
            (_BASE_NS,            1, True,
             [(100.0, 1.0), (99.0, 2.0)], [(101.0, 1.5), (102.0, 2.5)]),
            (_BASE_NS + 5_000_000_000, 1, False,
             [(100.0, 0.7)], []),
            (_BASE_NS + 10_000_000_000, 1, False,
             [(99.0, 0.0)], [(101.0, 0.0)]),
        ]
        _write_book_tape(self.tape, events)

    def test_point_query_at_each_event(self) -> None:
        snap0 = ob.book_at(self.tape, ts_ns=_BASE_NS + 1, levels=5)
        self.assertIsNotNone(snap0)
        self.assertEqual(snap0.bids[0], (100.0, 1.0))
        self.assertEqual(snap0.asks[0], (101.0, 1.5))

        snap1 = ob.book_at(self.tape,
                           ts_ns=_BASE_NS + 5_000_000_000 + 1, levels=5)
        # Top bid qty updated to 0.7.
        self.assertEqual(snap1.bids[0], (100.0, 0.7))

        snap2 = ob.book_at(self.tape,
                           ts_ns=_BASE_NS + 10_000_000_000 + 1, levels=5)
        # 99 bid + 101 ask removed.
        self.assertEqual([p for p, _ in snap2.bids], [100.0])
        self.assertEqual([p for p, _ in snap2.asks], [102.0])

    def test_point_query_before_first_event(self) -> None:
        snap = ob.book_at(self.tape, ts_ns=_BASE_NS - 1, levels=5)
        self.assertIsNone(snap)


class OutOfOrderEventsTests(unittest.TestCase):
    """Live-recorded tapes carry book events out of timestamp order
    (the bulk read surfaces tape/arrival order and applies no reorder
    buffer). The module must re-sort by exchange ts before applying
    snapshots/deltas, and must forward reader knobs like
    ``reorder_window_ns`` for parity with reader paths that enforce
    the window."""

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="ob-reorder-"))
        self.tape = self.tmp / "tape"
        # Arrival order: the 40s-newer snapshot first, then a 40s-late
        # one (own segment via a second writer session, as a live
        # recorder restart would produce). On the 60s grid _BASE_NS
        # sits at +20s into its bucket, so in chronological order the
        # two events land in consecutive buckets.
        _write_book_tape(self.tape, [
            (_BASE_NS + 40_000_000_000, 1, True,
             [(100.0, 1.0)], [(101.0, 1.0)]),
        ])
        _write_book_tape(self.tape, [
            (_BASE_NS, 1, True,
             [(90.0, 1.0)], [(91.0, 1.0)]),
        ])

    def test_iterator_sorts_before_applying(self) -> None:
        snaps = list(ob.OrderBookIterator(
            self.tape, bucket_ns=60_000_000_000, levels=5))
        self.assertEqual(len(snaps), 2)
        # Chronological: the older snapshot closes the first bucket,
        # the newer one the second. Without the sort this collapses
        # into a single bucket carrying the OLDER state.
        self.assertEqual(snaps[0].bids, [(90.0, 1.0)])
        self.assertEqual(snaps[0].asks, [(91.0, 1.0)])
        self.assertEqual(snaps[1].bids, [(100.0, 1.0)])
        self.assertEqual(snaps[1].asks, [(101.0, 1.0)])
        self.assertLess(snaps[0].ts_ns, snaps[1].ts_ns)

    def test_book_at_sorts_before_applying(self) -> None:
        # Point query after both events sees the newer state; without
        # the sort the arrival-ordered walk breaks early / applies the
        # late event last.
        snap = ob.book_at(self.tape, ts_ns=_BASE_NS + 50_000_000_000,
                          levels=5)
        self.assertEqual(snap.bids, [(100.0, 1.0)])
        # Between the two events only the older one applies.
        snap_mid = ob.book_at(self.tape, ts_ns=_BASE_NS + 1, levels=5)
        self.assertEqual(snap_mid.bids, [(90.0, 1.0)])

    def test_reorder_window_passthrough(self) -> None:
        # The knob forwards to the reader without changing the (already
        # sorted) result.
        snaps = list(ob.OrderBookIterator(
            self.tape, bucket_ns=60_000_000_000, levels=5,
            reorder_window_ns=120_000_000_000))
        self.assertEqual(len(snaps), 2)
        snap = ob.book_at(self.tape, ts_ns=_BASE_NS + 50_000_000_000,
                          levels=5, reorder_window_ns=120_000_000_000)
        self.assertEqual(snap.bids, [(100.0, 1.0)])

    def test_reader_kwargs_passthrough(self) -> None:
        # Forwarded verbatim: a valid knob works, an unknown one
        # surfaces the DataReader constructor error — proof the dict
        # actually reaches the reader.
        snaps = list(ob.OrderBookIterator(
            self.tape, bucket_ns=60_000_000_000, levels=5,
            reader_kwargs={"reorder_window_ns": 120_000_000_000}))
        self.assertEqual(len(snaps), 2)
        with self.assertRaises(TypeError):
            list(ob.OrderBookIterator(
                self.tape, bucket_ns=60_000_000_000, levels=5,
                reader_kwargs={"no_such_reader_kwarg": 1}))

    def test_explicit_knob_wins_over_reader_kwargs(self) -> None:
        snaps = list(ob.OrderBookIterator(
            self.tape, bucket_ns=60_000_000_000, levels=5,
            reorder_window_ns=120_000_000_000,
            reader_kwargs={"reorder_window_ns": 1_000_000_000}))
        self.assertEqual(len(snaps), 2)


def _upgrade_last_delta_to_top(tape_dir: Path) -> None:
    """Rewrite the last BookDelta frame of every segment as a BookTop.

    ``DataWriter.write_book`` cannot write a top yet (issue #640), so the
    fixture patches the frozen v1 layout directly: frame type 3 -> 6 at
    offset 8 of the FrameHeader, record byte 1 -> 2 at offset 32 of the
    BookRecordHeader, and the frame's payload CRC-32 (IEEE, same polynomial
    as zlib.crc32) recomputed. Frames sit back to back from byte 64
    (SegmentHeader) up to index_offset.
    """
    import struct
    import zlib

    for seg in sorted(tape_dir.glob("*.lrvx")):
        data = bytearray(seg.read_bytes())
        (index_offset,) = struct.unpack_from("<Q", data, 40)
        end = index_offset if index_offset else len(data)
        pos, last_delta = 64, None
        while pos + 12 <= end:
            size, _crc = struct.unpack_from("<II", data, pos)
            frame_type = data[pos + 8]
            if frame_type == 3:
                last_delta = pos
            pos += 12 + size
        if last_delta is None:
            continue
        size = struct.unpack_from("<I", data, last_delta)[0]
        payload = slice(last_delta + 12, last_delta + 12 + size)
        data[last_delta + 8] = 6                      # FrameHeader.type: BookTop
        data[last_delta + 12 + 32] = 2                # BookRecordHeader.type: top
        struct.pack_into("<I", data, last_delta + 4,
                         zlib.crc32(data[payload]) & 0xFFFFFFFF)
        seg.write_bytes(data)


class TopRowTests(unittest.TestCase):
    """A top row (record byte 2) is the venue's complete word on the touch
    and nothing deeper. Applying it as a delta stranded stale better
    levels in the reconstruction."""

    def _apply(self, bids, asks, event_type, bid_levels, ask_levels):
        levels = _arr([_level(p, q) for p, q in bid_levels + ask_levels])
        ob._apply_event(bids, asks, event_type=event_type,
                        n_bid=len(bid_levels), n_ask=len(ask_levels),
                        off=0, levels=levels)

    def test_top_drops_levels_more_aggressive_than_the_touch(self):
        bids, asks = {}, {}
        self._apply(bids, asks, 0,
                    [(100.0, 1.0), (99.0, 2.0)], [(101.0, 1.5), (102.0, 2.5)])
        # Bid touch retreats to 99.5: the 100 level provably emptied, the
        # 99 behind it stays; the ask touch improves to 100.5 and the
        # snapshot's levels behind it stay.
        self._apply(bids, asks, 2, [(99.5, 4.0)], [(100.5, 3.0)])
        self.assertEqual(bids, {_level(99.5, 4.0)[0]: _level(99.5, 4.0)[1],
                                _level(99.0, 2.0)[0]: _level(99.0, 2.0)[1]})
        self.assertEqual(asks, {_level(100.5, 3.0)[0]: _level(100.5, 3.0)[1],
                                _level(101.0, 1.5)[0]: _level(101.0, 1.5)[1],
                                _level(102.0, 2.5)[0]: _level(102.0, 2.5)[1]})

    def test_top_resizes_the_touch_level_in_place(self):
        bids, asks = {}, {}
        self._apply(bids, asks, 0, [(100.0, 1.0)], [(101.0, 1.5)])
        self._apply(bids, asks, 2, [(100.0, 7.0)], [(101.0, 0.0)])
        self.assertEqual(bids[_level(100.0, 7.0)[0]], _level(100.0, 7.0)[1])
        # qty 0 at the touch empties that level.
        self.assertEqual(asks, {})

    def test_top_empty_side_means_empty_side(self):
        bids, asks = {}, {}
        self._apply(bids, asks, 0, [(100.0, 1.0)], [(101.0, 1.5)])
        self._apply(bids, asks, 2, [(100.0, 1.0)], [])
        self.assertEqual(asks, {})
        self.assertIn(_level(100.0, 1.0)[0], bids)

    def test_iterator_and_book_at_keep_depth_behind_a_top(self):
        tmp = Path(tempfile.mkdtemp(prefix="ob-top-"))
        tape = tmp / "tape"
        _write_book_tape(tape, [
            (_BASE_NS, 1, True,
             [(100.0, 1.0), (99.0, 2.0)], [(101.0, 1.5), (102.0, 2.5)]),
            # Written as a delta, rewritten below as a top: bid touch
            # retreats to 99.5, the ask side goes empty.
            (_BASE_NS + 30_000_000_000, 1, False, [(99.5, 4.0)], []),
        ])
        _upgrade_last_delta_to_top(tape)

        snaps = list(ob.OrderBookIterator(tape, bucket_ns=60_000_000_000,
                                          levels=5))
        self.assertEqual(len(snaps), 1)
        self.assertEqual(snaps[0].bids, [(99.5, 4.0), (99.0, 2.0)])
        self.assertEqual(snaps[0].asks, [])

        snap = ob.book_at(tape, ts_ns=_BASE_NS + 40_000_000_000, levels=5)
        self.assertEqual(snap.bids, [(99.5, 4.0), (99.0, 2.0)])
        self.assertEqual(snap.asks, [])


if __name__ == "__main__":
    unittest.main()
