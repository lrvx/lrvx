"""OrderBookIterator round-trip — write a synthetic tape with a few
book events, iterate it at a 60s bucket cadence, then point-query
`book_at` at a chosen offset to confirm the reconstructed ladder.

CI-runnable companion to
[Iterate the order book from a tape](../how-to/iterate-orderbook.md).

Usage:
    cd /path/to/lrvx
    PYTHONPATH=build/python python3 docs/examples/python_orderbook_iterator.py
"""
from __future__ import annotations

import shutil
import tempfile
from pathlib import Path

import numpy as np

import lrvx
from lrvx import orderbook as ob

_LEVEL_DTYPE = np.dtype([
    ("price_raw", np.int64), ("qty_raw", np.int64), ("side", np.uint8),
])


def _arr(items):
    return np.array(
        [(int(round(p * 1e8)), int(round(q * 1e8)), 0) for p, q in items],
        dtype=_LEVEL_DTYPE,
    )


def main() -> None:
    tmp = Path(tempfile.mkdtemp(prefix="lrvx-ob-iter-"))
    try:
        tape = tmp / "tape"
        tape.mkdir()
        w = lrvx.DataWriter(str(tape), max_segment_mb=4,
                               exchange_id=0, compression="none")
        base = 1_700_000_000_000_000_000
        events = [
            # ts_offset_s, is_snapshot, bids, asks
            (0, True, [(100.0, 1.0), (99.0, 2.0)], [(101.0, 1.5), (102.0, 2.5)]),
            (30, False, [(100.0, 0.5)], []),
            (90, False, [(99.0, 0.0)], [(101.0, 0.0)]),
            (150, False, [(99.5, 1.2)], [(101.5, 0.4)]),
        ]
        for i, (off_s, is_snap, bids, asks) in enumerate(events):
            ts = base + off_s * 1_000_000_000
            w.write_book(exchange_ts_ns=ts, recv_ts_ns=ts,
                         seq=i, symbol_id=1, is_snapshot=is_snap,
                         bids=_arr(bids), asks=_arr(asks))
        w.close()

        # Iterate at 60s buckets, top-5 per side.
        print("OrderBookIterator(bucket=60s, levels=5):")
        for snap in ob.OrderBookIterator(tape, bucket_ns=60_000_000_000,
                                         levels=5):
            print(f"  ts={snap.ts_ns} bids[0]={snap.bids[0] if snap.bids else None} "
                  f"asks[0]={snap.asks[0] if snap.asks else None}")

        # Point query at a chosen instant.
        target = base + 75 * 1_000_000_000
        at = ob.book_at(tape, ts_ns=target, levels=5)
        assert at is not None
        print(f"book_at(ts=base+75s): bids[0]={at.bids[0]} "
              f"asks[0]={at.asks[0]}")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
