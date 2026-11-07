"""OrderBook snapshot iterator over a `.lrvx` tape.

Replays the book event stream of a tape and exposes the reconstructed
ladder state at bucket boundaries or arbitrary point-in-time queries.
The ladder is bounded to the requested ``levels`` per side; book
events that move the top of the deeper-than-N portion are still
applied, only the surfaced view is trimmed.

Two surfaces:

  * ``OrderBookIterator`` — iterable that yields ``BookSnapshot(ts_ns,
    bids, asks)`` once per ``bucket_ns`` window, capturing the latest
    ladder state observed inside that window.
  * ``book_at(tape, ts_ns, levels)`` — point query that walks the tape
    up to ``ts_ns`` and returns the latest ladder state.

Both go through ``DataReader.read_book_updates`` and the record's
snapshot / delta / top semantics; this module never touches the binary
format directly.

This is the pure-Python reference implementation — convenient for
point queries and hours-scale windows. For month-scale multi-symbol
feature extraction use ``lrvx.BookSnapshotBinAggregator`` via
``DataReader.run([...], n_threads=1)`` — same bucket semantics, C++
throughput.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterator, List, Optional, Tuple, Union

import numpy as np

PathLike = Union[str, Path]

# Kwargs forwarded verbatim to the internal ``DataReader`` on top of
# the time bounds this module manages itself.
ReaderKwargs = Optional[dict]


@dataclass
class BookSnapshot:
    """One snapshot of the reconstructed ladder.

    ``bids`` / ``asks`` are lists of ``(price, qty)`` floats sorted
    bids-descending and asks-ascending. ``symbol_id`` is the on-tape
    symbol id; the iterator emits one snapshot per (bucket, symbol).
    ``crossed`` is True when the best bid >= best ask at this
    snapshot — typically a momentary artifact of out-of-order book
    events; the caller can choose to drop the snapshot or proceed
    with the observation."""

    ts_ns: int
    symbol_id: int
    bids: List[Tuple[float, float]] = field(default_factory=list)
    asks: List[Tuple[float, float]] = field(default_factory=list)
    crossed: bool = False


def _to_array(ladder: dict, *, descending: bool, levels: int
              ) -> List[Tuple[float, float]]:
    """Trim the in-memory ladder to top-N levels and return a
    (price, qty) list in the requested direction."""
    if not ladder:
        return []
    items = sorted(ladder.items(),
                   key=lambda kv: -kv[0] if descending else kv[0])
    if levels and len(items) > levels:
        items = items[:levels]
    return [(p / 1e8, q / 1e8) for p, q in items]


def _read_events(tape_path: PathLike,
                 *,
                 t_from: Optional[int],
                 t_to: Optional[int],
                 reorder_window_ns: Optional[int] = None,
                 reader_kwargs: ReaderKwargs = None
                 ) -> Tuple[np.ndarray, np.ndarray]:
    """Wrapper around ``DataReader.read_book_updates`` that returns
    the structured (headers, levels) arrays, scoped by optional time
    bounds (half-open ``[t_from, t_to)``). Extra ``reader_kwargs``
    are forwarded to the ``DataReader`` constructor; the explicit
    ``reorder_window_ns`` knob wins over a value in ``reader_kwargs``."""
    import lrvx

    kwargs = dict(reader_kwargs) if reader_kwargs else {}
    if t_from is not None:
        kwargs["from_ns"] = int(t_from)
    if t_to is not None:
        kwargs["to_ns"] = int(t_to)
    if reorder_window_ns is not None:
        kwargs["reorder_window_ns"] = int(reorder_window_ns)
    reader = lrvx.DataReader(str(tape_path), **kwargs)
    headers, levels = reader.read_book_updates()
    # The bulk read surfaces events in tape/arrival order, which on
    # live-recorded tapes is not timestamp order. Snapshot/delta
    # application is order-sensitive, so re-sort by exchange ts
    # (stable: equal-ts events keep arrival order). Each header
    # carries its own level_offset, so the levels array stays as-is.
    if headers.size > 1:
        ts = headers["exchange_ts_ns"]
        if np.any(ts[1:] < ts[:-1]):
            headers = headers[np.argsort(ts, kind="stable")]
    return headers, levels


def _apply_touch(side: dict, *, more_aggressive, count: int, off: int,
                 levels: np.ndarray) -> None:
    """Apply one side of a top row to a ladder dict in place.

    ``more_aggressive`` compares a resting price against the touch price
    (``>`` for bids, ``<`` for asks). An empty side (``count == 0``) means
    that side of the book is empty.
    """
    if count == 0:
        side.clear()
        return
    p = int(levels[off]["price_raw"])
    q = int(levels[off]["qty_raw"])
    for stale in [s for s in side if more_aggressive(s, p)]:
        del side[stale]
    if q == 0:
        side.pop(p, None)
    else:
        side[p] = q


def _apply_event(bids: dict, asks: dict,
                 *, event_type: int,
                 n_bid: int, n_ask: int, off: int,
                 levels: np.ndarray) -> None:
    """Apply one book event to the running ladder dicts in place.

    ``event_type`` is the record byte: 0 snapshot, 1 delta, 2 top. A top
    is the venue's complete word on the touch and nothing deeper — levels
    more aggressive than it are gone, the touch level takes the reported
    size, deeper levels stay — the same reading the simulator's
    onTopOfBook and CompositeBookMatrix apply. Merging it as a delta
    strands stale better levels in the reconstruction.
    """
    if event_type == 2:
        _apply_touch(bids, more_aggressive=lambda s, p: s > p,
                     count=n_bid, off=off, levels=levels)
        _apply_touch(asks, more_aggressive=lambda s, p: s < p,
                     count=n_ask, off=off + n_bid, levels=levels)
        return
    if event_type == 0:
        bids.clear()
        asks.clear()
    for i in range(n_bid):
        p = int(levels[off + i]["price_raw"])
        q = int(levels[off + i]["qty_raw"])
        if q == 0:
            bids.pop(p, None)
        else:
            bids[p] = q
    for i in range(n_ask):
        p = int(levels[off + n_bid + i]["price_raw"])
        q = int(levels[off + n_bid + i]["qty_raw"])
        if q == 0:
            asks.pop(p, None)
        else:
            asks[p] = q


def _bucket_floor(ts_ns: int, bucket_ns: int) -> int:
    return (ts_ns // bucket_ns) * bucket_ns


class OrderBookIterator:
    """Yield reconstructed book snapshots once per ``bucket_ns`` window.

    Parameters
    ----------
    tape_path
        Path to the `.lrvx` directory.
    bucket_ns
        Snapshot cadence in nanoseconds. The yield carries the
        latest ladder state observed inside ``[bucket_start, bucket_end)``;
        windows with no book events are skipped.
    levels
        Maximum levels per side surfaced in each ``BookSnapshot``.
        Pass 0 to surface the full reconstructed ladder.
    t_from / t_to
        Optional half-open time bounds in nanoseconds.
    symbol_id
        Optional filter — yield snapshots only for this symbol id.
        Omit to yield one snapshot per (bucket, symbol) for every
        symbol present in the tape.
    reorder_window_ns
        Forwarded to the internal ``DataReader``. Out-of-order book
        events are handled here regardless (events are re-sorted by
        exchange ts before application); the knob is exposed for
        parity with reader paths that enforce the window.
    reader_kwargs
        Extra kwargs forwarded verbatim to the internal ``DataReader``
        (future reader knobs without one-by-one plumbing). Time bounds
        and ``reorder_window_ns`` set explicitly win over entries here.
    """

    def __init__(self,
                 tape_path: PathLike,
                 *,
                 bucket_ns: int,
                 levels: int = 20,
                 t_from: Optional[int] = None,
                 t_to: Optional[int] = None,
                 symbol_id: Optional[int] = None,
                 reorder_window_ns: Optional[int] = None,
                 reader_kwargs: ReaderKwargs = None) -> None:
        if bucket_ns <= 0:
            raise ValueError(f"bucket_ns must be > 0, got {bucket_ns}")
        if levels < 0:
            raise ValueError(f"levels must be >= 0, got {levels}")
        self._tape_path = tape_path
        self._bucket_ns = int(bucket_ns)
        self._levels = int(levels)
        self._t_from = t_from
        self._t_to = t_to
        self._symbol_id = symbol_id
        self._reorder_window_ns = reorder_window_ns
        self._reader_kwargs = reader_kwargs

    def __iter__(self) -> Iterator[BookSnapshot]:
        headers, levels_arr = _read_events(
            self._tape_path, t_from=self._t_from, t_to=self._t_to,
            reorder_window_ns=self._reorder_window_ns,
            reader_kwargs=self._reader_kwargs)
        if headers.size == 0:
            return
        # Per-symbol running ladder.
        running: dict = {}  # symbol_id → (bids dict, asks dict)
        # Per-symbol current bucket start.
        cur_bucket: dict = {}  # symbol_id → bucket_start_ns
        # Pending snapshot to emit when a new bucket boundary is seen
        # for a symbol.
        pending: dict = {}  # symbol_id → BookSnapshot
        levels_top = self._levels

        def _emit(sym_id: int) -> Optional[BookSnapshot]:
            snap = pending.pop(sym_id, None)
            if snap is None:
                return None
            bids_dict, asks_dict = running[sym_id]
            snap.bids = _to_array(bids_dict, descending=True, levels=levels_top)
            snap.asks = _to_array(asks_dict, descending=False, levels=levels_top)
            if snap.bids and snap.asks:
                snap.crossed = snap.bids[0][0] >= snap.asks[0][0]
            return snap

        for h in headers:
            sym = int(h["symbol_id"])
            if self._symbol_id is not None and sym != self._symbol_id:
                continue
            ts = int(h["exchange_ts_ns"])
            event_type = int(h["event_type"])
            off = int(h["level_offset"])
            n_bid = int(h["bid_count"])
            n_ask = int(h["ask_count"])

            if sym not in running:
                running[sym] = ({}, {})
            new_bucket = _bucket_floor(ts, self._bucket_ns)
            prev_bucket = cur_bucket.get(sym)

            # Crossing a bucket boundary closes the prior bucket
            # BEFORE this event mutates the ladder, so the emitted
            # snapshot reflects state at bucket_end (exclusive of
            # the first event of the next bucket).
            if prev_bucket is not None and new_bucket > prev_bucket:
                snap = _emit(sym)
                if snap is not None:
                    yield snap
                pending[sym] = BookSnapshot(ts_ns=new_bucket, symbol_id=sym)
            elif sym not in pending:
                pending[sym] = BookSnapshot(ts_ns=new_bucket, symbol_id=sym)

            bids_dict, asks_dict = running[sym]
            _apply_event(bids_dict, asks_dict,
                         event_type=event_type,
                         n_bid=n_bid, n_ask=n_ask, off=off,
                         levels=levels_arr)
            cur_bucket[sym] = new_bucket

        # Emit the trailing bucket for every symbol.
        for sym in list(pending.keys()):
            snap = _emit(sym)
            if snap is not None:
                yield snap


def book_at(tape_path: PathLike,
            *,
            ts_ns: int,
            levels: int = 20,
            symbol_id: Optional[int] = None,
            t_from: Optional[int] = None,
            reorder_window_ns: Optional[int] = None,
            reader_kwargs: ReaderKwargs = None) -> Optional[BookSnapshot]:
    """Reconstruct the latest book state at or before ``ts_ns``.

    Walks the tape from the optional ``t_from`` (or the tape start)
    up to ``ts_ns`` inclusive, applying snapshot / delta events to
    build the ladder. Returns ``None`` when the symbol has no book
    events in that range. When ``symbol_id`` is not set and the tape
    contains multiple symbols, returns the snapshot for whichever
    symbol carried the most recent book event before ``ts_ns``.
    """
    headers, levels_arr = _read_events(tape_path, t_from=t_from, t_to=ts_ns + 1,
                                       reorder_window_ns=reorder_window_ns,
                                       reader_kwargs=reader_kwargs)
    if headers.size == 0:
        return None
    running: dict = {}
    last_ts: dict = {}
    for h in headers:
        sym = int(h["symbol_id"])
        if symbol_id is not None and sym != symbol_id:
            continue
        ts = int(h["exchange_ts_ns"])
        if ts > ts_ns:
            break
        if sym not in running:
            running[sym] = ({}, {})
        bids_dict, asks_dict = running[sym]
        _apply_event(bids_dict, asks_dict,
                     event_type=int(h["event_type"]),
                     n_bid=int(h["bid_count"]),
                     n_ask=int(h["ask_count"]),
                     off=int(h["level_offset"]),
                     levels=levels_arr)
        last_ts[sym] = ts

    if not running:
        return None

    if symbol_id is not None:
        sym = symbol_id
    else:
        sym = max(last_ts, key=lambda s: last_ts[s])
    bids_dict, asks_dict = running[sym]
    snap = BookSnapshot(
        ts_ns=last_ts[sym],
        symbol_id=sym,
        bids=_to_array(bids_dict, descending=True, levels=levels),
        asks=_to_array(asks_dict, descending=False, levels=levels),
    )
    if snap.bids and snap.asks:
        snap.crossed = snap.bids[0][0] >= snap.asks[0][0]
    return snap


__all__ = [
    "BookSnapshot",
    "OrderBookIterator",
    "book_at",
]
