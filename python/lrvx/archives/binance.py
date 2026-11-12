"""Binance public archive → lrvx converter.

``data.binance.vision`` publishes one aggTrades zip per UTC day for
spot, USDT-margined futures (``um-futures``) and coin-margined futures
(``cm-futures``):

    https://data.binance.vision/data/spot/daily/aggTrades/<SYM>/<SYM>-aggTrades-<YYYY-MM-DD>.zip
    https://data.binance.vision/data/futures/um/daily/aggTrades/<SYM>/...
    https://data.binance.vision/data/futures/cm/daily/aggTrades/<SYM>/...

The layout is recognised per file:

  * futures files from mid 2022 on open with the header
    ``agg_trade_id,price,quantity,first_trade_id,last_trade_id,transact_time,is_buyer_maker``;
    older futures files have the same seven columns and no header;
  * spot files have no header and eight columns, the same seven plus
    ``is_best_match``.

Any other first row is rejected with ``UnknownLayoutError``.

The timestamp is Unix milliseconds, except in spot files from
2025-01-01 on, where Binance writes microseconds. A value of 10^14 or
more is read as microseconds (10^14 ms is the year 5138), so both land
on the tape as the same nanoseconds.

Quantity is stored as published: the base asset for spot and
um-futures, contracts for cm-futures (``BTCUSD_PERP``: 100 USD per
contract, other cm symbols 10 USD). The live path through the ccxt
bridge reports cm contracts in contracts too. ``qty_multiplier``
scales every quantity by a constant the caller supplies; the importer
does not know contract sizes. ``metadata.json`` records the unit under
``custom.qty_unit`` and the multiplier under ``custom.qty_multiplier``.

Side encoding follows the existing lrvx convention:

  * ``is_buyer_maker = true``  → buyer rests as maker → active flow is
    selling → ``Side::SELL`` (1)
  * ``is_buyer_maker = false`` → seller rests as maker → active flow is
    buying  → ``Side::BUY``  (0)

Days are UTC days: the file for 2024-01-15 holds the trades from
2024-01-15T00:00Z to 2024-01-16T00:00Z.

The range form caches the downloaded files under
``<cache_root>/binance/<market>/<symbol>/`` (``mirror`` replaces
``<cache_root>/binance``), with the download policy of ``_download``.

The converter writes through ``lrvx.DataWriter`` and is append-safe:
a row is skipped when its ``agg_trade_id`` (stable, exchange-assigned)
is already on the tape for the same symbol within the incoming file's
time range, so re-running a day is a no-op and days may be imported
in any order.

A standalone ``metadata.json`` is written alongside the segments so the
result keys cleanly into ``MergedTapeReader`` against other lrvx
tapes — exchange = "binance", instrument_type derived from ``market``.
"""
from __future__ import annotations

import json
from dataclasses import dataclass
from datetime import date, datetime, timezone
from pathlib import Path
from typing import Iterator, List, Optional, Tuple, Union

from . import _cache, _dedup, _layout, _range
from ._download import evict_if_corrupt
from ._download import fetch as _download

# Market identifier → URL path prefix on data.binance.vision for the
# trade stream (aggTrades) and the book stream (bookTicker). Layout in
# the public archive:
#
#   data/{market_segment}/daily/{product}/{symbol}/{symbol}-{product}-{date}.zip
#
# where market_segment is one of `spot`, `futures/um`, `futures/cm`.
_MARKET_SEGMENTS = {
    "spot": "data/spot/daily",
    "um-futures": "data/futures/um/daily",
    "cm-futures": "data/futures/cm/daily",
}

# Backwards-compat alias used by the existing aggTrades importer.
_MARKET_PATHS = {
    seg: f"{path}/aggTrades" for seg, path in _MARKET_SEGMENTS.items()
}

# Book archive product names on data.binance.vision, by book_type.
# `bookTicker` is best bid/ask per update.
_BOOK_PRODUCT_NAMES = {
    "t1": "bookTicker",
}

# Markets whose archive publishes each book product. The spot archive
# has no bookTicker product.
_BOOK_MARKETS = {
    "t1": {"um-futures", "cm-futures"},
}

_BASE_URL = "https://data.binance.vision"

# Side values match lrvx::Side (BUY=0, SELL=1) as written by the
# in-engine recorder hook. Keep these locked to the C++ enum so the
# converter and the live recorder produce equivalent tapes.
_SIDE_BUY = 0
_SIDE_SELL = 1

# instrument_type stamped into metadata.json. Mirrors the wording
# the live binance connector uses so MergedTapeReader buckets converge.
_INSTRUMENT_TYPE = {
    "spot": "spot",
    "um-futures": "perpetual",
    "cm-futures": "perpetual",
}

DateLike = Union[str, date]

# The archive's days start at midnight UTC.
DAY_UTC_OFFSET_HOURS = 0


@dataclass
class ConvertStats:
    """Per-invocation summary returned by the converters."""

    trades_written: int = 0
    rows_read: int = 0
    rows_skipped: int = 0
    rows_invalid: int = 0
    files_processed: int = 0
    first_ts_ns: int = 0
    last_ts_ns: int = 0
    last_trade_id: int = 0


# ── HTTP / mirror helpers ───────────────────────────────────────────


def _aggtrades_url(symbol: str, market: str, day: date) -> str:
    if market not in _MARKET_PATHS:
        raise ValueError(
            f"unknown market '{market}'. "
            f"expected one of: {sorted(_MARKET_PATHS)}"
        )
    prefix = _MARKET_PATHS[market]
    return f"{_BASE_URL}/{prefix}/{symbol}/{symbol}-aggTrades-{day.isoformat()}.zip"


def _mirror_path(mirror: Path, symbol: str, market: str, day: date) -> Path:
    return mirror / market / symbol / f"{symbol}-aggTrades-{day.isoformat()}.zip"


def check_symbol(symbol: str, market: str) -> None:
    """Raise ValueError unless ``symbol`` has the shape of a Binance
    ``market`` symbol as the archive names it. A local check; no
    request is made."""
    if market not in _MARKET_SEGMENTS:
        raise ValueError(
            f"unknown market '{market}'. expected one of {sorted(_MARKET_SEGMENTS)}")
    head, sep, tail = symbol.partition("_")
    if market == "spot":
        ok = _range.is_upper_alnum(symbol)
        expected, example = "the pair in upper case with no separator", "BTCUSDT"
    elif market == "um-futures":
        ok = _range.is_upper_alnum(head) and (
            not sep or (len(tail) == 6 and tail.isdigit()))
        expected = ("the contract in upper case, optionally with a delivery "
                    "date suffix")
        example = "BTCUSDT or BTCUSDT_240329"
    else:
        ok = (_range.is_upper_alnum(head) and head.endswith("USD") and len(head) > 3
              and (tail == "PERP" or (len(tail) == 6 and tail.isdigit())))
        expected = "a coin-margined contract name, <COIN>USD_PERP or <COIN>USD_<YYMMDD>"
        example = "BTCUSD_PERP or BTCUSD_240329"
    if not ok:
        raise _range.bad_symbol("Binance", market, symbol, expected, example)


# ── CSV parsing ─────────────────────────────────────────────────────


_AGG_COLUMNS = ("agg_trade_id", "price", "quantity", "first_trade_id",
                "last_trade_id", "transact_time", "is_buyer_maker")

_AGG_LAYOUTS = (
    _layout.Layout("futures", _AGG_COLUMNS),
    _layout.Layout("futures, no header", _AGG_COLUMNS, has_header=False),
    _layout.Layout("spot", _AGG_COLUMNS + ("is_best_match",), has_header=False),
)

# Timestamps at or above this are microseconds (spot files from
# 2025-01-01); below it, milliseconds.
_MICROS_FROM = 10 ** 14


def _ts_ns(token: str) -> int:
    ts = int(token)
    return ts * 1_000 if ts >= _MICROS_FROM else ts * 1_000_000


def _maker_flag(token: str) -> bool:
    t = token.strip().lower()
    if t in ("true", "1"):
        return True
    if t in ("false", "0"):
        return False
    raise ValueError(token)


def _iter_rows(src: _layout.CsvFile, qty_multiplier: float = 1.0
               ) -> Iterator[Tuple[int, float, float, int, bool]]:
    """Yield ``(agg_id, price, qty, ts_ns, is_buyer_maker)`` rows from a
    Binance aggTrades CSV (zipped or extracted)."""
    for row in src.rows():
        try:
            yield (int(row[0]), float(row[1]), float(row[2]) * qty_multiplier,
                   _ts_ns(row[5]), _maker_flag(row[6]))
        except (ValueError, IndexError):
            src.counts.bad(row)


# ── metadata.json writer ────────────────────────────────────────────


def _ns_to_iso(ns: int) -> str:
    if ns <= 0:
        return ""
    secs, rem = divmod(int(ns), 1_000_000_000)
    dt = datetime.fromtimestamp(secs, tz=timezone.utc)
    return dt.strftime("%Y-%m-%dT%H:%M:%S.") + f"{rem // 1_000_000:03d}Z"


def _base_quote(name: str) -> Tuple[str, str]:
    for quote in ("USDT", "USDC", "BUSD", "BTC", "ETH", "BNB"):
        if name.endswith(quote) and len(name) > len(quote):
            return name[: -len(quote)], quote
    return "", ""


def _write_metadata(
    out_tape: Path,
    *,
    exchange_name: str,
    market: str,
    symbol_id: int,
    symbol_name: str,
    first_ts_ns: int,
    last_ts_ns: int,
    trades_added: int,
    qty_multiplier: float = 1.0,
) -> None:
    """Create or merge ``metadata.json`` so MergedTapeReader can key
    this tape by ``(exchange, name)``."""
    base, quote = _base_quote(symbol_name)
    sym_entry = {
        "symbol_id": int(symbol_id),
        "name": symbol_name,
        "base_asset": base,
        "quote_asset": quote,
        "price_precision": 8,
        "qty_precision": 8,
    }
    meta = {
        "recording_id": f"binance-{market}-{symbol_name}",
        "description": f"Binance {market} aggTrades archive for {symbol_name}",
        "exchange": exchange_name,
        "exchange_type": "cex",
        "instrument_type": _INSTRUMENT_TYPE.get(market, ""),
        "connector_version": "binance-archive-import",
        "symbols": [sym_entry],
        "has_trades": trades_added > 0,
        "has_book_snapshots": False,
        "has_book_deltas": False,
        "total_trades": int(trades_added),
        "total_book_updates": 0,
        "book_depth": 0,
        "recording_start": _ns_to_iso(first_ts_ns),
        "recording_end": _ns_to_iso(last_ts_ns),
        "price_scale": 100_000_000,
        "qty_scale": 100_000_000,
        "hostname": "",
        "timezone": "UTC",
        "lrvx_version": "",
        "custom": {
            "qty_unit": "contracts" if market == "cm-futures" else "base",
            "qty_multiplier": float(qty_multiplier),
        },
    }

    meta_path = out_tape / "metadata.json"
    if meta_path.exists():
        try:
            existing = json.loads(meta_path.read_text())
            # Merge: widen time range, sum trades, dedup symbols.
            old_start = existing.get("recording_start") or meta["recording_start"]
            old_end = existing.get("recording_end") or meta["recording_end"]
            if meta["recording_start"] and (not old_start or meta["recording_start"] < old_start):
                old_start = meta["recording_start"]
            if meta["recording_end"] and meta["recording_end"] > old_end:
                old_end = meta["recording_end"]
            existing["recording_start"] = old_start
            existing["recording_end"] = old_end
            existing["total_trades"] = int(existing.get("total_trades", 0)) + int(trades_added)
            existing["has_trades"] = existing["total_trades"] > 0
            seen = {int(s["symbol_id"]): s for s in existing.get("symbols", [])}
            seen[int(symbol_id)] = sym_entry
            existing["symbols"] = list(seen.values())
            # Keep the rest of existing fields (exchange / exchange_type /
            # instrument_type) untouched if already set sensibly.
            for k in ("exchange", "exchange_type", "instrument_type",
                      "connector_version", "price_scale", "qty_scale",
                      "timezone"):
                if not existing.get(k):
                    existing[k] = meta[k]
            custom = existing.get("custom") or {}
            custom.update(meta["custom"])
            existing["custom"] = custom
            meta = existing
        except (OSError, ValueError, KeyError):
            pass

    out_tape.mkdir(parents=True, exist_ok=True)
    meta_path.write_text(json.dumps(meta, indent=2, sort_keys=True))


# ── Single-day conversion ───────────────────────────────────────────


def aggtrades_to_lrvx(
    csv_path: Union[str, Path],
    out_tape: Union[str, Path],
    *,
    symbol_id: int = 1,
    symbol_name: str = "",
    market: str = "um-futures",
    exchange_id: int = 0,
    exchange_name: str = "binance",
    append: bool = True,
    max_segment_mb: int = 256,
    compression: str = "none",
    write_metadata: bool = True,
    qty_multiplier: float = 1.0,
) -> ConvertStats:
    """Convert one Binance aggTrades day (zipped or extracted CSV) into
    a lrvx tape.

    ``symbol_name`` defaults to the file's basename leading token if
    omitted. Dedup keys on ``agg_trade_id`` within the file's time
    range, so re-running a day already present in ``out_tape`` returns
    ``trades_written=0`` and updates only ``rows_skipped``. Raises
    ``UnknownLayoutError`` for a first row that is neither the futures
    header nor a headerless seven-column (futures) or eight-column
    (spot) row, and ``NothingParsedError`` when the file has rows and
    none parse.
    """
    if market not in _MARKET_PATHS:
        raise ValueError(
            f"unknown market '{market}'. expected one of: {sorted(_MARKET_PATHS)}")
    csv_path = Path(csv_path).expanduser()
    out_tape = Path(out_tape).expanduser()
    if not csv_path.exists():
        raise FileNotFoundError(csv_path)
    if not symbol_name:
        # "BTCUSDT-aggTrades-2024-01-15.zip" → "BTCUSDT"
        symbol_name = csv_path.stem.split("-", 1)[0]

    # Rows keep file order; the side is resolved from is_buyer_maker as
    # they are parsed.
    src = _layout.CsvFile(csv_path, _AGG_LAYOUTS, source="Binance aggTrades")
    parsed = ((agg_id, price, qty, ts_ns, _SIDE_SELL if maker else _SIDE_BUY)
              for agg_id, price, qty, ts_ns, maker
              in _iter_rows(src, qty_multiplier))
    with _dedup.TradeFile(parsed, _dedup.trade_dtype("i8"), sort=False) as rows:
        src.check(rows.rows_read)
        done = _dedup.write_trade_file(
            rows, out_tape, append=append, symbol_id=symbol_id,
            exchange_id=exchange_id, max_segment_mb=max_segment_mb,
            compression=compression)
        stats = ConvertStats(
            trades_written=done.count,
            rows_read=rows.rows_read,
            rows_skipped=rows.rows_skipped,
            rows_invalid=src.counts.invalid,
            files_processed=1,
            first_ts_ns=done.first_ts_ns,
            last_ts_ns=done.last_ts_ns,
            last_trade_id=done.last_id if done.count else rows.id_max,
        )

    if write_metadata and (stats.trades_written or stats.rows_read):
        _write_metadata(
            out_tape,
            exchange_name=exchange_name,
            market=market,
            symbol_id=symbol_id,
            symbol_name=symbol_name,
            first_ts_ns=stats.first_ts_ns,
            last_ts_ns=stats.last_ts_ns,
            trades_added=stats.trades_written,
            qty_multiplier=qty_multiplier,
        )
    return stats


# ── Multi-day conversion ────────────────────────────────────────────


def range_to_lrvx(
    symbol: str,
    market: str,
    date_from: DateLike,
    date_to: DateLike,
    out_tape: Union[str, Path],
    *,
    mirror: Optional[Union[str, Path]] = None,
    parallel: int = 4,
    symbol_id: int = 1,
    exchange_id: int = 0,
    exchange_name: str = "binance",
    append: bool = True,
    max_segment_mb: int = 256,
    compression: str = "none",
    skip_missing: bool = False,
    qty_multiplier: float = 1.0,
) -> ConvertStats:
    """Convert a range of UTC days (inclusive) into lrvx.

    Files are taken from, and missing ones downloaded into, the cache:
    ``<cache_root>/binance`` (``~/.lrvx/archive-cache/binance`` unless
    ``LRVX_ARCHIVE_CACHE`` says otherwise), or ``mirror`` when given,
    laid out as ``<market>/<symbol>/<symbol>-aggTrades-<YYYY-MM-DD>.zip``.

    The symbol's shape is checked before any request. A range in which
    no day is published raises ``NoFilesError``; a missing day raises
    ``FileNotFoundError`` unless ``skip_missing``. Downloads run up to
    ``parallel`` connections in flight; the write step is serial so
    the writer stays append-safe.
    """
    check_symbol(symbol, market)
    days = _range.days_between(date_from, date_to)
    if not days:
        return ConvertStats()
    mirror = _cache.exchange_dir("binance", mirror)

    def _ensure(day: date) -> Path:
        local = _mirror_path(mirror, symbol, market, day)
        if not local.exists():
            _download(_aggtrades_url(symbol, market, day), local)
        return local

    pairs = _range.fetch_days(days, _ensure, parallel=parallel,
                              skip_missing=skip_missing,
                              what=f"binance {market} aggTrades {symbol}")

    agg = ConvertStats()
    for day, path in pairs:
        if path is None:
            continue
        with evict_if_corrupt(path):
            # Per-day metadata is skipped; one write at the end records
            # the whole range and total count.
            s = aggtrades_to_lrvx(
                path, out_tape, symbol_id=symbol_id, symbol_name=symbol,
                market=market, exchange_id=exchange_id,
                exchange_name=exchange_name, append=append,
                max_segment_mb=max_segment_mb, compression=compression,
                write_metadata=False, qty_multiplier=qty_multiplier)
        agg.trades_written += s.trades_written
        agg.rows_read += s.rows_read
        agg.rows_skipped += s.rows_skipped
        agg.rows_invalid += s.rows_invalid
        agg.files_processed += s.files_processed
        if s.first_ts_ns and (agg.first_ts_ns == 0 or s.first_ts_ns < agg.first_ts_ns):
            agg.first_ts_ns = s.first_ts_ns
        if s.last_ts_ns > agg.last_ts_ns:
            agg.last_ts_ns = s.last_ts_ns
        if s.last_trade_id > agg.last_trade_id:
            agg.last_trade_id = s.last_trade_id

    _write_metadata(
        Path(out_tape),
        exchange_name=exchange_name,
        market=market,
        symbol_id=symbol_id,
        symbol_name=symbol,
        first_ts_ns=agg.first_ts_ns,
        last_ts_ns=agg.last_ts_ns,
        trades_added=agg.trades_written,
        qty_multiplier=qty_multiplier,
    )

    return agg


UnknownLayoutError = _layout.UnknownLayoutError
NothingParsedError = _layout.NothingParsedError

__all__ = [
    "ConvertStats",
    "BookConvertStats",
    "NothingParsedError",
    "UnknownLayoutError",
    "aggtrades_to_lrvx",
    "check_symbol",
    "range_to_lrvx",
    "t1_to_lrvx",
    "range_book_to_lrvx",
]


# ════════════════════════════════════════════════════════════════════
# Book archive (T1 / bookTicker)
# ════════════════════════════════════════════════════════════════════
#
# bookTicker (book_type "t1") is the best bid / ask per update, one row
# per tick, published on data.binance.vision for um-futures and
# cm-futures.
#
# The converter shares the parse / write spine of the aggTrades
# importer above (zip member open, append-safe dedup, metadata merge).
# Book events are delta-encoded against the previous event of the same
# file, and every file opens with a snapshot (see _BookSink), so the
# resulting lrvx plays back through MergedTapeReader and the
# existing book aggregator surface without modification.


@dataclass
class BookConvertStats:
    """Per-invocation summary for the book converters."""

    snapshots_written: int = 0
    deltas_written: int = 0
    rows_read: int = 0
    rows_skipped: int = 0
    rows_invalid: int = 0
    files_processed: int = 0
    first_ts_ns: int = 0
    last_ts_ns: int = 0
    last_update_id: int = 0


def _aggregate_book_stats(agg: BookConvertStats, add: BookConvertStats) -> None:
    agg.snapshots_written += add.snapshots_written
    agg.deltas_written += add.deltas_written
    agg.rows_read += add.rows_read
    agg.rows_skipped += add.rows_skipped
    agg.rows_invalid += add.rows_invalid
    agg.files_processed += add.files_processed
    if add.first_ts_ns and (agg.first_ts_ns == 0 or add.first_ts_ns < agg.first_ts_ns):
        agg.first_ts_ns = add.first_ts_ns
    if add.last_ts_ns > agg.last_ts_ns:
        agg.last_ts_ns = add.last_ts_ns
    if add.last_update_id > agg.last_update_id:
        agg.last_update_id = add.last_update_id


def _check_book_request(book_type: str, market: str) -> None:
    """Raise ValueError unless Binance publishes ``book_type`` for
    ``market``."""
    if book_type not in _BOOK_PRODUCT_NAMES:
        raise ValueError(
            f"unsupported book_type '{book_type}': the only book archive "
            f"imported is 't1' (bookTicker, best bid/ask)"
        )
    if market not in _MARKET_SEGMENTS:
        raise ValueError(
            f"unknown market '{market}'. expected one of {sorted(_MARKET_SEGMENTS)}"
        )
    if market not in _BOOK_MARKETS[book_type]:
        raise ValueError(
            f"Binance publishes no {_BOOK_PRODUCT_NAMES[book_type]} archive "
            f"for market '{market}'; book imports support "
            f"{sorted(_BOOK_MARKETS[book_type])}"
        )


def _book_archive_url(symbol: str, market: str, book_type: str, day: date) -> str:
    _check_book_request(book_type, market)
    seg = _MARKET_SEGMENTS[market]
    product = _BOOK_PRODUCT_NAMES[book_type]
    return f"{_BASE_URL}/{seg}/{product}/{symbol}/{symbol}-{product}-{day.isoformat()}.zip"


def _book_mirror_path(mirror: Path, symbol: str, market: str,
                      book_type: str, day: date) -> Path:
    product = _BOOK_PRODUCT_NAMES[book_type]
    return mirror / market / symbol / f"{symbol}-{product}-{day.isoformat()}.zip"


def _file_ts_range_ms(path: Path, *, ts_col: int) -> Optional[Tuple[int, int]]:
    """Smallest and largest timestamp (ms) in column ``ts_col`` of a
    book archive file, or None when no row parses."""
    lo: Optional[int] = None
    hi: Optional[int] = None
    for row in _open_csv_lines(path):
        try:
            ts = int(row[ts_col])
        except (ValueError, IndexError):
            continue
        if lo is None or ts < lo:
            lo = ts
        if hi is None or ts > hi:
            hi = ts
    return None if lo is None else (lo, hi)


class _BookSink:
    """Writes one file's book records, leaving out those already on the
    tape.

    Every imported file is encoded on its own: the first record is a
    snapshot and each delta is taken against the record before it in
    the same file, never against what the tape held. A file therefore
    always yields the same records whatever else is on the tape, and a
    record is on the tape already exactly when a record with its
    ``seq`` is, for the same symbol, inside the file's time range.
    That bounded membership check is the whole dedup, so days land in
    any order and a re-import or an overlapping range adds nothing."""

    _BATCH = 4096

    def __init__(self, out_tape: Path, *, symbol_id: int, exchange_id: int,
                 max_segment_mb: int, compression: str,
                 on_tape: "object") -> None:
        self._out_tape = out_tape
        self._symbol_id = int(symbol_id)
        self._exchange_id = int(exchange_id)
        self._max_segment_mb = int(max_segment_mb)
        self._compression = compression
        self._on_tape = on_tape
        self._pending: List[Tuple[int, int, bool, list, list, int]] = []
        self._writer = None
        self.snapshots = 0
        self.deltas = 0
        self.rows_on_tape = 0
        self.first_ts_ns = 0
        self.last_ts_ns = 0
        self.last_seq = 0

    def emit(self, ts_ns: int, seq: int, is_snapshot: bool,
             bids: List[Tuple[int, int]], asks: List[Tuple[int, int]],
             n_rows: int) -> None:
        self._pending.append((ts_ns, seq, is_snapshot, bids, asks, n_rows))
        if len(self._pending) >= self._BATCH:
            self._flush()

    def _flush(self) -> None:
        import numpy as np

        pending, self._pending = self._pending, []
        if not pending:
            return
        if self._on_tape.size:
            seqs = np.fromiter((e[1] for e in pending), dtype=np.uint64,
                               count=len(pending))
            dup = np.isin(seqs, self._on_tape).tolist()
        else:
            dup = [False] * len(pending)
        for (ts_ns, seq, is_snapshot, bids, asks, n_rows), seen in zip(pending, dup):
            if seen:
                self.rows_on_tape += n_rows
                continue
            self._write(ts_ns, seq, is_snapshot, bids, asks)

    def _write(self, ts_ns: int, seq: int, is_snapshot: bool,
               bids: List[Tuple[int, int]], asks: List[Tuple[int, int]]) -> None:
        if self._writer is None:
            import lrvx

            self._out_tape.mkdir(parents=True, exist_ok=True)
            self._writer = lrvx.DataWriter(
                str(self._out_tape),
                max_segment_mb=self._max_segment_mb,
                exchange_id=self._exchange_id,
                compression=self._compression,
            )
        self._writer.write_book(
            exchange_ts_ns=ts_ns, recv_ts_ns=ts_ns,
            seq=int(seq), symbol_id=self._symbol_id,
            is_snapshot=is_snapshot,
            bids=_levels_array(bids), asks=_levels_array(asks),
        )
        if is_snapshot:
            self.snapshots += 1
        else:
            self.deltas += 1
        self.first_ts_ns = self.first_ts_ns or ts_ns
        self.last_ts_ns = ts_ns
        self.last_seq = int(seq)

    def close(self) -> None:
        try:
            self._flush()
        finally:
            if self._writer is not None:
                self._writer.close()
                self._writer = None


def _open_book_sink(csv_path: Path, out_tape: Path, *, ts_col: int,
                    append: bool, symbol_id: int,
                    exchange_id: int, max_segment_mb: int,
                    compression: str) -> _BookSink:
    import numpy as np

    on_tape = np.empty(0, dtype=np.uint64)
    if append:
        span = _file_ts_range_ms(csv_path, ts_col=ts_col)
        if span is not None:
            on_tape = _dedup.existing_book_seqs(
                out_tape, symbol_id,
                span[0] * 1_000_000, span[1] * 1_000_000)
    return _BookSink(out_tape, symbol_id=symbol_id, exchange_id=exchange_id,
                     max_segment_mb=max_segment_mb, compression=compression,
                     on_tape=on_tape)


def _levels_array(items: List[Tuple[int, int]]):
    """Build a PyLevel structured numpy array from (price_raw, qty_raw)
    tuples. The DataWriter ignores the trailing side byte on write."""
    import numpy as np

    dtype = np.dtype([
        ("price_raw", np.int64),
        ("qty_raw", np.int64),
        ("side", np.uint8),
    ])
    arr = np.zeros(len(items), dtype=dtype)
    for i, (p, q) in enumerate(items):
        arr[i]["price_raw"] = p
        arr[i]["qty_raw"] = q
    return arr


def _diff_book(prev_bids: dict, cur_bids: dict,
               prev_asks: dict, cur_asks: dict) -> Tuple[List[Tuple[int, int]],
                                                          List[Tuple[int, int]]]:
    """Compare two ladders (dict[price_raw → qty_raw]) and return the
    delta levels to emit. Removed price levels carry qty=0; new and
    changed levels carry the current qty."""
    bid_delta: List[Tuple[int, int]] = []
    for p, q in cur_bids.items():
        if prev_bids.get(p) != q:
            bid_delta.append((p, q))
    for p in prev_bids.keys() - cur_bids.keys():
        bid_delta.append((p, 0))
    ask_delta: List[Tuple[int, int]] = []
    for p, q in cur_asks.items():
        if prev_asks.get(p) != q:
            ask_delta.append((p, q))
    for p in prev_asks.keys() - cur_asks.keys():
        ask_delta.append((p, 0))
    return bid_delta, ask_delta


_BOOK_COLUMNS = ("update_id", "best_bid_price", "best_bid_qty",
                 "best_ask_price", "best_ask_qty", "transaction_time",
                 "event_time")

_BOOK_LAYOUTS = (
    _layout.Layout("bookTicker", _BOOK_COLUMNS),
)


def _book_csv(path: Path) -> _layout.CsvFile:
    return _layout.CsvFile(path, _BOOK_LAYOUTS, source="Binance bookTicker")


def _open_csv_lines(path: Path) -> Iterator[List[str]]:
    """Iterate the data rows of a bookTicker file (zipped or extracted),
    its header row recognised and left out. Raises UnknownLayoutError
    for a first row that is not the bookTicker header."""
    return _book_csv(path).rows()


# ── T1 / bookTicker ────────────────────────────────────────────────


def t1_to_lrvx(
    csv_path: Union[str, Path],
    out_tape: Union[str, Path],
    *,
    symbol_id: int = 1,
    symbol_name: str = "",
    market: str = "um-futures",
    exchange_id: int = 0,
    exchange_name: str = "binance",
    append: bool = True,
    max_segment_mb: int = 256,
    compression: str = "none",
    write_metadata: bool = True,
) -> BookConvertStats:
    """Convert one Binance bookTicker day (zipped or extracted CSV) into
    book events on the lrvx tape. Each row maps to one top-of-book
    snapshot (1 bid level + 1 ask level). Consecutive rows where the
    full top did not change are skipped — `OrderBookIterator` callers
    only need to see ticks that actually move the book.

    Binance bookTicker CSV columns (post-2023):

        update_id, best_bid_price, best_bid_qty, best_ask_price,
        best_ask_qty, transaction_time, event_time

    The file is encoded on its own: its first change is written as a
    snapshot and later ones as deltas against the row before, so the
    records a day yields do not depend on what the tape already holds.
    With ``append`` a record is left out when a record with its
    ``update_id`` is on the tape for ``symbol_id`` inside the file's
    time range; days may be imported in any order and re-imports add
    nothing.

    ``market`` is ``"um-futures"`` or ``"cm-futures"``; any other value
    raises ValueError.
    """
    _check_book_request("t1", market)
    csv_path = Path(csv_path).expanduser()
    out_tape = Path(out_tape).expanduser()
    if not csv_path.exists():
        raise FileNotFoundError(csv_path)
    if not symbol_name:
        symbol_name = csv_path.stem.split("-", 1)[0]

    sink = _open_book_sink(csv_path, out_tape, ts_col=5,
                           append=append, symbol_id=symbol_id,
                           exchange_id=exchange_id,
                           max_segment_mb=max_segment_mb,
                           compression=compression)

    src = _book_csv(csv_path)
    rows_read = 0
    rows_skipped = 0
    prev_bids: dict = {}
    prev_asks: dict = {}
    is_first_emit = True
    try:
        for row in src.rows():
            rows_read += 1
            try:
                update_id = int(row[0])
                bid_p = float(row[1])
                bid_q = float(row[2])
                ask_p = float(row[3])
                ask_q = float(row[4])
                ts_ms = int(row[5])
            except (ValueError, IndexError):
                src.counts.bad(row)
                rows_skipped += 1
                continue

            bid_p_raw = int(round(bid_p * 1e8))
            bid_q_raw = int(round(bid_q * 1e8))
            ask_p_raw = int(round(ask_p * 1e8))
            ask_q_raw = int(round(ask_q * 1e8))

            cur_bids = {bid_p_raw: bid_q_raw} if bid_q_raw > 0 else {}
            cur_asks = {ask_p_raw: ask_q_raw} if ask_q_raw > 0 else {}

            if cur_bids == prev_bids and cur_asks == prev_asks:
                # Top-of-book unchanged; skip to keep the tape small.
                rows_skipped += 1
                continue

            ts_ns = ts_ms * 1_000_000

            if is_first_emit:
                sink.emit(ts_ns, update_id, True, list(cur_bids.items()),
                          list(cur_asks.items()), 1)
                is_first_emit = False
            else:
                bid_delta, ask_delta = _diff_book(
                    prev_bids, cur_bids, prev_asks, cur_asks)
                sink.emit(ts_ns, update_id, False, bid_delta, ask_delta, 1)

            prev_bids = cur_bids
            prev_asks = cur_asks
    finally:
        sink.close()
    src.check(rows_read - src.counts.invalid)

    return _finish_book_import(
        sink, out_tape, rows_read=rows_read, rows_skipped=rows_skipped,
        rows_invalid=src.counts.invalid,
        write_metadata=write_metadata, exchange_name=exchange_name,
        market=market, symbol_id=symbol_id, symbol_name=symbol_name,
        depth=1)


def _finish_book_import(sink: _BookSink, out_tape: Path, *, rows_read: int,
                        rows_skipped: int, rows_invalid: int,
                        write_metadata: bool,
                        exchange_name: str, market: str, symbol_id: int,
                        symbol_name: str, depth: int) -> BookConvertStats:
    stats = BookConvertStats(
        snapshots_written=sink.snapshots,
        deltas_written=sink.deltas,
        rows_read=rows_read,
        rows_skipped=rows_skipped + sink.rows_on_tape,
        rows_invalid=rows_invalid,
        files_processed=1,
        first_ts_ns=sink.first_ts_ns,
        last_ts_ns=sink.last_ts_ns,
        last_update_id=sink.last_seq,
    )
    total_writes = sink.snapshots + sink.deltas
    if write_metadata and total_writes > 0:
        _write_book_metadata(
            out_tape,
            exchange_name=exchange_name,
            market=market,
            symbol_id=symbol_id,
            symbol_name=symbol_name,
            first_ts_ns=sink.first_ts_ns,
            last_ts_ns=sink.last_ts_ns,
            updates_added=total_writes,
            depth=depth,
        )
    return stats


def _write_book_metadata(out_tape: Path, *,
                         exchange_name: str,
                         market: str,
                         symbol_id: int,
                         symbol_name: str,
                         first_ts_ns: int,
                         last_ts_ns: int,
                         updates_added: int,
                         depth: int) -> None:
    """Merge book counters into the tape's metadata.json. Re-uses the
    aggTrades writer (`_write_metadata`) for the time-range / symbol
    plumbing; adds the book-specific counters on top."""
    _write_metadata(
        out_tape,
        exchange_name=exchange_name,
        market=market,
        symbol_id=symbol_id,
        symbol_name=symbol_name,
        first_ts_ns=first_ts_ns,
        last_ts_ns=last_ts_ns,
        trades_added=0,
    )
    meta_path = out_tape / "metadata.json"
    if not meta_path.exists():
        return
    try:
        meta = json.loads(meta_path.read_text())
    except (OSError, ValueError):
        return
    meta["has_book_snapshots"] = True
    meta["has_book_deltas"] = True
    meta["total_book_updates"] = int(meta.get("total_book_updates", 0)) + int(updates_added)
    if int(meta.get("book_depth", 0)) < depth:
        meta["book_depth"] = int(depth)
    meta_path.write_text(json.dumps(meta, indent=2, sort_keys=True))


# ── Multi-day book range ────────────────────────────────────────────


def range_book_to_lrvx(
    symbol: str,
    market: str,
    book_type: str,
    date_from: DateLike,
    date_to: DateLike,
    out_tape: Union[str, Path],
    *,
    mirror: Optional[Union[str, Path]] = None,
    parallel: int = 2,
    symbol_id: int = 1,
    exchange_id: int = 0,
    exchange_name: str = "binance",
    append: bool = True,
    max_segment_mb: int = 256,
    compression: str = "none",
    skip_missing: bool = False,
) -> BookConvertStats:
    """Convert a range of UTC days (inclusive) of a book archive into
    lrvx book events. Files are cached like the aggTrades range form's,
    in ``<cache_root>/binance`` or ``mirror``, as
    ``<market>/<symbol>/<symbol>-bookTicker-<YYYY-MM-DD>.zip``. Downloads
    run up to ``parallel`` connections in flight (default 2); conversion
    is serial so the writer stays append-safe.

    ``book_type`` is ``"t1"`` (bookTicker) and ``market`` is
    ``"um-futures"`` or ``"cm-futures"``; anything else, or a symbol of
    the wrong shape, raises ValueError before any download. Missing
    days are handled as in ``range_to_lrvx``."""
    _check_book_request(book_type, market)
    check_symbol(symbol, market)

    days = _range.days_between(date_from, date_to)
    if not days:
        return BookConvertStats()
    mirror = _cache.exchange_dir("binance", mirror)

    def _ensure(day: date) -> Path:
        local = _book_mirror_path(mirror, symbol, market, book_type, day)
        if not local.exists():
            _download(_book_archive_url(symbol, market, book_type, day), local)
        return local

    pairs = _range.fetch_days(
        days, _ensure, parallel=parallel, skip_missing=skip_missing,
        what=f"binance {market} {_BOOK_PRODUCT_NAMES[book_type]} {symbol}")

    agg = BookConvertStats()
    for day, path in pairs:
        if path is None:
            continue
        with evict_if_corrupt(path):
            stats = t1_to_lrvx(
                path, out_tape,
                symbol_id=symbol_id, symbol_name=symbol, market=market,
                exchange_id=exchange_id, exchange_name=exchange_name,
                append=append, max_segment_mb=max_segment_mb,
                compression=compression, write_metadata=False,
            )
        _aggregate_book_stats(agg, stats)

    if agg.snapshots_written + agg.deltas_written > 0:
        _write_book_metadata(
            Path(out_tape),
            exchange_name=exchange_name,
            market=market,
            symbol_id=symbol_id,
            symbol_name=symbol,
            first_ts_ns=agg.first_ts_ns,
            last_ts_ns=agg.last_ts_ns,
            updates_added=agg.snapshots_written + agg.deltas_written,
            depth=1,
        )

    return agg
