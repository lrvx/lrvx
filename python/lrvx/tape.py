"""Tape recording / replay primitives backing ``lrvx tape``.

``.lrvx`` is lrvx's on-disk format for deterministic event capture.
This module wraps ``lrvx.DataWriter`` / ``DataReader`` (the C++
writer/reader) and ``lrvx.BinaryLogRecorderHook`` for the
``lrvx tape record`` and ``lrvx tape replay`` CLI subcommands in
:mod:`lrvx.cli`.

Trades and book updates are both written. The hook stays in C++ on the
hot path, so there's no Python callback per event.
"""
from __future__ import annotations

import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, List, Optional


@dataclass
class TapeRecorderStats:
    trades_written: int = 0
    book_updates_written: int = 0
    bytes_written: int = 0
    segments_created: int = 0
    errors: int = 0


def make_recorder_hook(
    output_dir: Path,
    *,
    max_segment_mb: int = 256,
    exchange_id: int = 0,
    compression: str = "none",
    exchange_name: str = "",
    instrument_type: str = "",
) -> Any:
    """Return a :class:`lrvx.BinaryLogRecorderHook` ready to plug
    into ``runner.set_market_data_recorder(hook)``. Writes both trades
    and book updates to ``output_dir`` via the in-tree
    ``BinaryLogWriter``.

    ``exchange_name`` and ``instrument_type`` are stamped into the
    tape's ``metadata.json`` so :class:`lrvx.MergedTapeReader` can
    key it by ``(exchange, name)`` against other tapes — leaving them
    empty produces a tape that merges into the unnamed-exchange bucket.

    Differs from subclassing :class:`lrvx.MarketDataRecorderHook` in
    three ways: no Python callback per event, raw int64 prices/quantities
    (no float64 round-trip), and book capture, not trades only.
    """
    import lrvx

    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    return lrvx.BinaryLogRecorderHook(
        str(output_dir),
        max_segment_mb=int(max_segment_mb),
        exchange_id=int(exchange_id),
        compression=compression,
        exchange_name=exchange_name,
        instrument_type=instrument_type,
    )


def _hook_stats_to_dataclass(stats_dict: dict) -> TapeRecorderStats:
    return TapeRecorderStats(
        trades_written=int(stats_dict.get("trades_written", 0)),
        book_updates_written=int(stats_dict.get("book_updates_written", 0)),
        bytes_written=int(stats_dict.get("bytes_written", 0)),
        segments_created=int(stats_dict.get("segments_created", 0)),
        errors=int(stats_dict.get("errors", 0)),
    )


# ── Replay helpers ──────────────────────────────────────────────────


@dataclass
class TapeStats:
    """Result of :func:`inspect_tape`."""
    path: str
    trade_count: int
    first_ts_ns: int
    last_ts_ns: int
    symbol_ids: List[int] = field(default_factory=list)


def inspect_tape(path: str | Path) -> TapeStats:
    """Open a ``.lrvx`` directory and return summary statistics
    without dispatching events through an Engine. Used by
    ``lrvx tape inspect`` and as a smoke check for replay-equivalence.
    """
    import lrvx
    import numpy as np

    p = str(path)
    reader = lrvx.DataReader(p)
    trades = reader.read_trades()
    if trades.size == 0:
        return TapeStats(path=p, trade_count=0, first_ts_ns=0, last_ts_ns=0)
    ts = np.asarray(trades["exchange_ts_ns"])
    sym = np.asarray(trades["symbol_id"])
    return TapeStats(
        path=p,
        trade_count=int(ts.size),
        first_ts_ns=int(ts[0]),
        last_ts_ns=int(ts[-1]),
        symbol_ids=sorted(int(s) for s in np.unique(sym).tolist()),
    )


def replay_tape(
    path: str | Path,
    *,
    on_trade: Optional[Any] = None,
) -> int:
    """Iterate trades from a ``.lrvx`` directory, optionally
    invoking ``on_trade(timestamp_ns, symbol_id, price, qty, side)``
    for each row.

    Returns the number of trades dispatched. Order is exchange
    timestamp ascending — same order the engine saw them live, which
    is what makes replay-equivalence testable.
    """
    import lrvx

    reader = lrvx.DataReader(str(path))
    trades = reader.read_trades()
    n = int(trades.size)
    if on_trade is None or n == 0:
        return n
    for row in trades:
        on_trade(
            int(row["exchange_ts_ns"]),
            int(row["symbol_id"]),
            float(row["price_raw"]) / 1e8,
            float(row["qty_raw"]) / 1e8,
            int(row["side"]),
        )
    return n


@dataclass
class TapeDiff:
    """Result of :func:`diff_tapes`. ``equal`` is true when both tapes
    have the same trade count and every paired record matches."""

    left_path: str
    right_path: str
    left_count: int
    right_count: int
    first_divergence_index: Optional[int]
    mismatches: List[dict] = field(default_factory=list)
    equal: bool = False

    def to_dict(self) -> dict:
        return {
            "left_path": self.left_path,
            "right_path": self.right_path,
            "left_count": self.left_count,
            "right_count": self.right_count,
            "first_divergence_index": self.first_divergence_index,
            "mismatches": list(self.mismatches),
            "equal": self.equal,
        }


def diff_tapes(
    left: str | Path,
    right: str | Path,
    *,
    max_mismatches: int = 16,
    field_tolerance_ns: int = 0,
) -> TapeDiff:
    """Compare two ``.lrvx`` directories trade by trade. Returns
    ``equal=True`` when both have the same count and every paired
    record matches on ``(exchange_ts_ns, symbol_id, price_raw,
    qty_raw, side)``. ``field_tolerance_ns`` allows a non-zero
    timestamp jitter, useful when comparing live captures whose
    record-side wallclock differs across runs.

    The first ``max_mismatches`` divergences are recorded; the rest
    are summarized by count. Pass a high value if you need the full
    list. The walk runs in the C++ engine; this wrapper marshals
    the result into the existing ``TapeDiff`` dataclass shape."""
    from lrvx._lrvx import _tape_diff_native  # type: ignore[attr-defined]

    left = Path(left).expanduser()
    right = Path(right).expanduser()

    raw = _tape_diff_native(
        left=str(left),
        right=str(right),
        max_mismatches=int(max_mismatches),
        field_tolerance_ns=int(field_tolerance_ns),
    )
    return TapeDiff(
        left_path=raw["left_path"],
        right_path=raw["right_path"],
        left_count=int(raw["left_count"]),
        right_count=int(raw["right_count"]),
        first_divergence_index=raw["first_divergence_index"],
        mismatches=list(raw["mismatches"]),
        equal=bool(raw["equal"]),
    )


@dataclass
class MultiTapeStats:
    """Result of :func:`replay_tapes`. ``trades`` / ``books`` are totals
    across all tapes after merge-sort; ``tapes`` is a per-tape
    contribution list: ``{path, trades, books, first_event_ns,
    last_event_ns}`` — useful for debugging "one of the tapes is empty"."""

    trades: int = 0
    books: int = 0
    tapes: List[dict] = field(default_factory=list)


def replay_tapes(
    paths: List[str | Path],
    *,
    on_trade: Optional[Any] = None,
    on_book: Optional[Any] = None,
    from_ns: Optional[int] = None,
    to_ns: Optional[int] = None,
    symbols: Optional[List[int]] = None,
) -> MultiTapeStats:
    """Replay the merged event stream from N ``.lrvx`` directories.

    Symbols are rekeyed into a global id space keyed by
    ``(metadata.exchange, name)``. Events emit in ``exchange_ts_ns``
    order; ties break by ``paths`` order.

    Callbacks (any may be ``None``):

      ``on_trade(ts_ns, global_symbol_id, price, qty, side)``
      ``on_book(ts_ns, global_symbol_id, is_snapshot, bids, asks)``
        — ``bids`` / ``asks`` are lists of ``(price, qty)`` tuples.
    """
    import lrvx

    paths = [str(Path(p).expanduser()) for p in paths]
    reader = lrvx.MergedTapeReader(
        paths,
        from_ns=from_ns,
        to_ns=to_ns,
        symbols=symbols,
    )

    trades = reader.read_trades()
    headers, levels = reader.read_books()

    if on_trade is not None and trades.size:
        for row in trades:
            on_trade(
                int(row["exchange_ts_ns"]),
                int(row["symbol_id"]),
                float(row["price_raw"]) / 1e8,
                float(row["qty_raw"]) / 1e8,
                int(row["side"]),
            )

    if on_book is not None and headers.size:
        for h in headers:
            offset = int(h["level_offset"])
            n_bid = int(h["bid_count"])
            n_ask = int(h["ask_count"])
            if int(h["event_type"]) == 6:
                # A top-of-book frame; on_book is bool-shaped.
                continue
            bids = [
                (float(levels[offset + i]["price_raw"]) / 1e8,
                 float(levels[offset + i]["qty_raw"]) / 1e8)
                for i in range(n_bid)
            ]
            asks = [
                (float(levels[offset + n_bid + i]["price_raw"]) / 1e8,
                 float(levels[offset + n_bid + i]["qty_raw"]) / 1e8)
                for i in range(n_ask)
            ]
            on_book(
                int(h["exchange_ts_ns"]),
                int(h["symbol_id"]),
                int(h["event_type"]) == 2,
                bids,
                asks,
            )

    return MultiTapeStats(
        trades=int(trades.size),
        books=int(headers.size),
        tapes=list(reader.per_tape_stats()),
    )


__all__ = [
    "TapeRecorderStats",
    "TapeStats",
    "TapeDiff",
    "MultiTapeStats",
    "make_recorder_hook",
    "inspect_tape",
    "replay_tape",
    "replay_tapes",
    "diff_tapes",
]
