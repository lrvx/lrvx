"""Append dedup shared by the trade importers.

An incoming row is skipped when its trade id is already on the tape
for the same symbol within the incoming file's time range. A trade
keeps its exchange timestamp, so a copy of it on the tape can only sit
inside that range; the tape read is bounded to it, which keeps each
import at roughly one file's worth of tape instead of the whole tape.

Membership, not ``id <= max``, so days imported out of order, and a
day filling a gap between days already on the tape, land in full.
"""
from __future__ import annotations

from pathlib import Path
from typing import List, Sequence, Tuple, TypeVar

Row = TypeVar("Row", bound=tuple)


def existing_trade_ids(out_tape: Path, symbol_id: int,
                       from_ns: int, to_ns: int) -> "object":
    """Return the trade ids on the tape for ``symbol_id`` with an
    exchange timestamp in ``[from_ns, to_ns]``, as a numpy uint64 array
    (empty when the tape is missing or unreadable)."""
    import numpy as np

    empty = np.empty(0, dtype=np.uint64)
    if not out_tape.exists():
        return empty
    try:
        import lrvx

        reader = lrvx.DataReader(str(out_tape), from_ns=int(from_ns),
                                 to_ns=int(to_ns), symbols=[int(symbol_id)])
        trades = reader.read_trades()
        if trades.size == 0:
            return empty
        return np.asarray(trades["trade_id"], dtype=np.uint64)
    except Exception:
        return empty


def drop_trades_on_tape(rows: Sequence[Row], out_tape: Path, symbol_id: int,
                        *, ts_ns_per_unit: int = 1) -> Tuple[List[Row], int]:
    """Split ``rows`` into the ones not yet on the tape and a skip count.

    Each row carries its trade id at index 0 and its exchange timestamp
    at index 3, in units of ``ts_ns_per_unit`` nanoseconds."""
    import numpy as np

    rows = list(rows)
    if not rows:
        return rows, 0
    stamps = [r[3] for r in rows]
    existing = existing_trade_ids(out_tape, symbol_id,
                                  min(stamps) * ts_ns_per_unit,
                                  max(stamps) * ts_ns_per_unit)
    if existing.size == 0:
        return rows, 0
    incoming = np.fromiter((r[0] for r in rows), dtype=np.uint64,
                           count=len(rows))
    seen = np.isin(incoming, existing)
    kept = [r for r, dup in zip(rows, seen.tolist()) if not dup]
    return kept, len(rows) - len(kept)
