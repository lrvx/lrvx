"""The touch on the Python ``SymbolContext``, end to end from a tape.

A TOP_OF_BOOK frame moves ``best_bid`` / ``best_ask`` and ``last_touch_ns``
without firing ``on_book_update``; ``last_update_ns`` stays on the last
dispatched event. ``DataWriter.write_book`` cannot write a top yet, so the
fixture rewrites a delta frame in place: frame type 3 -> 6 at offset 8 of
the FrameHeader, record byte 1 -> 2 at offset 32 of the BookRecordHeader,
payload CRC-32 recomputed. Frames sit back to back from byte 64
(SegmentHeader) up to ``index_offset`` (u64 at byte 40); see
docs/spec/lrvx.md.

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_touch_context.py
"""
from __future__ import annotations

import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
for cand in ("build/python", "build-py312/python"):
    p = REPO_ROOT / cand
    if p.is_dir():
        sys.path.insert(0, str(p))
        break

import numpy as np  # noqa: E402

import lrvx  # noqa: E402

_LEVEL_DTYPE = np.dtype([
    ("price_raw", np.int64),
    ("qty_raw", np.int64),
    ("side", np.uint8),
])

SNAP_NS = 1_700_000_000_000_000_000
TOP_NS = SNAP_NS + 1_000_000_000
TRADE_NS = TOP_NS + 1_000_000_000


def _levels(items):
    return np.array([(int(round(p * 1e8)), int(round(q * 1e8)), 0) for p, q in items],
                    dtype=_LEVEL_DTYPE)


def _rewrite_last_delta_as_top(tape_dir: Path) -> int:
    """Turn the last BookDelta frame of every segment into a BookTop.

    Returns the number of frames rewritten, so a fixture that silently
    rewrote nothing fails loudly instead of testing a delta."""
    rewritten = 0
    for seg in sorted(tape_dir.glob("*.lrvx")):
        data = bytearray(seg.read_bytes())
        (index_offset,) = struct.unpack_from("<Q", data, 40)
        end = index_offset if index_offset else len(data)
        pos, last_delta = 64, None
        while pos + 12 <= end:
            (size,) = struct.unpack_from("<I", data, pos)
            if data[pos + 8] == 3:
                last_delta = pos
            pos += 12 + size
        if last_delta is None:
            continue
        (size,) = struct.unpack_from("<I", data, last_delta)
        payload = slice(last_delta + 12, last_delta + 12 + size)
        data[last_delta + 8] = 6          # FrameHeader.type: BookTop
        data[last_delta + 12 + 32] = 2    # BookRecordHeader.type: top-of-book
        struct.pack_into("<I", data, last_delta + 4, zlib.crc32(data[payload]) & 0xFFFFFFFF)
        seg.write_bytes(data)
        rewritten += 1
    return rewritten


def _write_tape(tape_dir: Path, symbol_id: int) -> None:
    tape_dir.mkdir(parents=True, exist_ok=True)
    w = lrvx.DataWriter(str(tape_dir), max_segment_mb=4, exchange_id=0, compression="none")
    try:
        w.write_book(exchange_ts_ns=SNAP_NS, recv_ts_ns=SNAP_NS, seq=1, symbol_id=symbol_id,
                     is_snapshot=True,
                     bids=_levels([(100.0, 1.0), (99.0, 2.0)]),
                     asks=_levels([(101.0, 1.0), (102.0, 2.0)]))
        # Written as a delta, rewritten below as a top: the touch tightens to
        # 100.4 / 100.6 while the depth book keeps the snapshot.
        w.write_book(exchange_ts_ns=TOP_NS, recv_ts_ns=TOP_NS, seq=2, symbol_id=symbol_id,
                     is_snapshot=False,
                     bids=_levels([(100.4, 3.0)]),
                     asks=_levels([(100.6, 2.0)]))
        # This strategy has no on_touch, so a trade after the top is the
        # first callback to see the context the top left behind.
        w.write_trade(exchange_ts_ns=TRADE_NS, recv_ts_ns=TRADE_NS, price=100.5, qty=0.1,
                      trade_id=1, symbol_id=symbol_id, side=0)
    finally:
        w.close()
    assert _rewrite_last_delta_as_top(tape_dir) == 1


class _Watcher(lrvx.Strategy):
    def __init__(self, symbols):
        super().__init__(symbols)
        self.at_book: list[tuple] = []
        self.at_trade: list[tuple] = []

    def on_book_update(self, ctx):
        self.at_book.append((ctx.last_update_ns, ctx.last_touch_ns, ctx.best_bid))

    def on_trade(self, ctx, trade):
        self.at_trade.append((ctx.last_update_ns, ctx.last_touch_ns, ctx.best_bid, ctx.best_ask))


class TouchContext(unittest.TestCase):
    def test_top_moves_the_touch_and_its_stamp_without_a_callback(self):
        tmp = Path(tempfile.mkdtemp(prefix="lrvx-touch-ctx-"))
        registry = lrvx.SymbolRegistry()
        sym = registry.add_symbol("sim", "BTCUSDT", tick_size=0.01)
        _write_tape(tmp / "tape", int(sym))

        runner = lrvx.BacktestRunner(registry, fee_rate=0.0, initial_capital=100_000.0)
        strat = _Watcher([sym])
        runner.set_strategy(strat)
        runner.run_tape(str(tmp / "tape"))

        # The depth frame dispatched once and stamped both timestamps.
        self.assertEqual(strat.at_book, [(SNAP_NS, SNAP_NS, 100.0)])
        # At the trade, last_update_ns is the trade's stamp, last_touch_ns is
        # still the top's, and the quotes are the top's: the callback-side
        # context tells the two stamps apart.
        self.assertEqual(strat.at_trade, [(TRADE_NS, TOP_NS, 100.4, 100.6)])

        # The pull-side context after the run says the same.
        ctx = strat.ctx(sym)
        self.assertEqual(ctx.best_bid, 100.4)
        self.assertEqual(ctx.best_ask, 100.6)
        self.assertEqual(ctx.last_touch_ns, TOP_NS)
        self.assertEqual(ctx.last_update_ns, TRADE_NS)
        self.assertEqual(ctx.mid_price, 100.5)
        del runner


if __name__ == "__main__":
    unittest.main()
