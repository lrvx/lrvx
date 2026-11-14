"""Bybit public archive → lrvx converter.

Bybit publishes daily trade files at ``https://public.bybit.com/``,
which serves a directory listing of every symbol and day. One file per
UTC day:

  * spot:    ``public.bybit.com/spot/<SYM>/<SYM>_<YYYY-MM-DD>.csv.gz``
    (BTCUSDT from 2022-11-10)
  * linear:  ``public.bybit.com/trading/<SYM>/<SYM><YYYY-MM-DD>.csv.gz``
    (BTCUSDT from 2020-03-25)
  * inverse: ``public.bybit.com/trading/<SYM>/<SYM><YYYY-MM-DD>.csv.gz``

Linear and inverse share the ``trading/`` prefix and differ only in
symbol naming: linear ``BTCUSDT`` (USDT perpetual), ``BTCPERP`` (USDC
perpetual) and ``BTC-27DEC24`` (USDC dated future); inverse ``BTCUSD``
and ``BTCUSDZ24`` (dated). The symbol is used verbatim after a shape
check against these forms.

Days are UTC days. The range form caches the downloaded files under
``<cache_root>/bybit/<market>/<symbol>/`` (``mirror`` replaces
``<cache_root>/bybit``), with the download policy of ``_download``.

The CSV layout is read from the header row. Derivatives files:

    timestamp,symbol,side,size,price,tickDirection,trdMatchID,
    grossValue,homeNotional,foreignNotional[,RPI]

``timestamp`` is Unix seconds with a decimal fraction; ``side`` is the
taker side, ``Buy`` / ``Sell``; ``trdMatchID`` is a UUID, folded into
the lrvx ``trade_id``. ``size`` is in the base coin for linear and in
USD contracts (1 USD each) for inverse.

Spot files:

    id,timestamp,price,volume,side[,rpi]

``timestamp`` is Unix milliseconds; ``side`` is ``buy`` / ``sell``;
``volume`` is in the base coin; ``id`` counts the day's trades from 1
and is used as the lrvx ``trade_id``.

Quantity is stored as published (the live Bybit connector does the
same). ``metadata.json`` records the unit under ``custom.qty_unit``.
Any other header is rejected with ``UnknownLayoutError``.

With ``append`` on, a row is skipped when its id is already on the
tape for the same symbol within the incoming file's time range, so
re-running a day is a no-op and a new day lands in full. Older files
list trades newest first; every file is written in timestamp order,
ties in file order.
"""
from __future__ import annotations

import itertools
import json
import re
from hashlib import md5 as _md5
from dataclasses import dataclass
from datetime import date, datetime, timezone
from pathlib import Path
from typing import Iterator, Optional, Tuple, Union

from . import _cache, _dedup, _layout, _range
from ._download import evict_if_corrupt
from ._download import fetch as _download


_BASE_URL = "https://public.bybit.com"

_MARKET_PATHS = {
    "spot":    "spot",
    "linear":  "trading",
    "inverse": "trading",
}

_INSTRUMENT_TYPE = {
    "spot":    "spot",
    "linear":  "perpetual",
    "inverse": "perpetual",
}

_TRADING_COLUMNS = ("timestamp", "symbol", "side", "size", "price",
                    "tickdirection", "trdmatchid", "grossvalue",
                    "homenotional", "foreignnotional")
_SPOT_COLUMNS = ("id", "timestamp", "price", "volume", "side")

_LAYOUTS = {
    "spot": (
        _layout.Layout("spot", _SPOT_COLUMNS),
        _layout.Layout("spot+rpi", _SPOT_COLUMNS + ("rpi",)),
    ),
    "trading": (
        _layout.Layout("trading", _TRADING_COLUMNS),
        _layout.Layout("trading+rpi", _TRADING_COLUMNS + ("rpi",)),
    ),
}

_QTY_UNIT = {"spot": "base", "linear": "base", "inverse": "usd_contracts"}

DateLike = Union[str, date]

# The archive's days start at midnight UTC.
DAY_UTC_OFFSET_HOURS = 0


@dataclass
class ConvertStats:
    trades_written: int = 0
    rows_read: int = 0
    rows_skipped: int = 0
    rows_invalid: int = 0
    files_processed: int = 0
    first_ts_ns: int = 0
    last_ts_ns: int = 0
    last_trade_id: int = 0


def _market_path(market: str) -> str:
    if market not in _MARKET_PATHS:
        raise ValueError(
            f"unknown market '{market}'. expected one of {sorted(_MARKET_PATHS)}"
        )
    return _MARKET_PATHS[market]


def _file_name(symbol: str, market: str, day: date) -> str:
    sep = "_" if _market_path(market) == "spot" else ""
    return f"{symbol}{sep}{day.isoformat()}.csv.gz"


def _archive_url(symbol: str, market: str, day: date) -> str:
    seg = _market_path(market)
    return f"{_BASE_URL}/{seg}/{symbol}/{_file_name(symbol, market, day)}"


def _mirror_path(mirror: Path, symbol: str, market: str, day: date) -> Path:
    return mirror / market / symbol / _file_name(symbol, market, day)


_INVERSE_DATED = re.compile(r"[A-Z0-9]+USD[FGHJKMNQUVXZ]\d{2}")
_LINEAR_DATED = re.compile(r"[A-Z0-9_]+-\d{2}[A-Z]{3}\d{2}")


def check_symbol(symbol: str, market: str) -> None:
    """Raise ValueError unless ``symbol`` has the shape of a Bybit
    ``market`` symbol as the archive names it. A local check; no
    request is made."""
    _market_path(market)
    alnum = _range.is_upper_alnum(symbol)
    if market == "spot":
        ok = alnum
        expected, example = "the pair in upper case with no separator", "BTCUSDT"
    elif market == "linear":
        ok = ((alnum and symbol.endswith(("USDT", "PERP")) and len(symbol) > 4)
              or bool(_LINEAR_DATED.fullmatch(symbol)))
        expected = ("<COIN>USDT, <COIN>PERP (USDC perpetual) or "
                    "<NAME>-<DDMMMYY> (dated contract)")
        example = "BTCUSDT, BTCPERP or BTC-27DEC24"
    else:
        ok = ((alnum and symbol.endswith("USD") and len(symbol) > 3)
              or bool(_INVERSE_DATED.fullmatch(symbol)))
        expected = "<COIN>USD or <COIN>USD<month code><YY> (dated)"
        example = "BTCUSD or BTCUSDZ24"
    if not ok:
        raise _range.bad_symbol("Bybit", market, symbol, expected, example)


def _hash_trade_id(token: str) -> int:
    """Bybit's `trdMatchID` is a UUID string. Fold it to an unsigned
    64-bit int, the first 8 bytes of its MD5 digest read big-endian, so
    the lrvx ``trade_id`` field (uint64) can carry it. The folded ids are unordered, so append dedup checks
    membership, never ``<= max``. Collisions are vanishingly rare
    within a single symbol's daily volume; one would only matter when
    a new trade collides with a trade already on the tape in the same
    time window, which skips the new trade."""
    # A UUID has '-' at index 8 and no leading whitespace, which int()
    # can never parse; skip the failing attempt for it.
    if not (len(token) > 8 and token[8] == "-" and not token[0].isspace()):
        try:
            return int(token)
        except (ValueError, TypeError):
            pass
    h = _md5(token.encode("ascii", errors="ignore")).digest()
    return int.from_bytes(h[:8], "big", signed=False)


def _parse_ts_ns(token: str) -> int:
    """Parse a Bybit timestamp string (Unix seconds, dot-decimal) into
    nanoseconds without losing precision to float round-off. Accepts
    ``1700000001.25`` as well as ``1700000001.250000000``."""
    secs_str, dot, frac_str = token.strip().partition(".")
    if dot:
        # Pad / truncate the fractional part to 9 digits (ns).
        if len(frac_str) > 9:
            frac_str = frac_str[:9]
        else:
            frac_str = frac_str.ljust(9, "0")
        return int(secs_str) * 1_000_000_000 + int(frac_str)
    return int(secs_str) * 1_000_000_000


# The side tokens as the archive spells them; anything else goes through
# _layout.side_of.
_SIDES = {"Buy": 0, "Sell": 1, "buy": 0, "sell": 1}


def _iter_rows(src: _layout.CsvFile) -> Iterator[Tuple[int, float, float, int, int]]:
    """Yield ``(trade_id, price, qty, ts_ns, side)`` rows from a Bybit
    archive CSV, spot or derivatives, by its detected layout."""
    rows = src.rows()
    for first in rows:
        # The layout is known once the first data row is read.
        spot = src.layout.columns[0] == "id"
        yield from (_iter_spot if spot else _iter_trading)(
            src, itertools.chain((first,), rows))
        return


def _iter_spot(src: _layout.CsvFile, rows) -> Iterator[Tuple[int, float, float, int, int]]:
    sides, side_of, bad = _SIDES, _layout.side_of, src.counts.bad
    for row in rows:
        try:
            side = sides.get(row[4])
            if side is None:
                side = side_of(row[4])
                if side is None:
                    raise ValueError(row[4])
            yield (int(row[0]), float(row[2]), float(row[3]),
                   int(row[1]) * 1_000_000, side)
        except (ValueError, IndexError):
            bad(row)


def _iter_trading(src: _layout.CsvFile, rows) -> Iterator[Tuple[int, float, float, int, int]]:
    sides, side_of, bad = _SIDES, _layout.side_of, src.counts.bad
    trade_id, ts_ns = _hash_trade_id, _parse_ts_ns
    for row in rows:
        try:
            side = sides.get(row[2])
            if side is None:
                side = side_of(row[2])
            if side is None or not row[6]:
                raise ValueError(row[2])
            yield (trade_id(row[6]), float(row[4]), float(row[3]),
                   ts_ns(row[0]), side)
        except (ValueError, IndexError):
            bad(row)


def _ns_to_iso(ns: int) -> str:
    if ns <= 0:
        return ""
    secs, rem = divmod(int(ns), 1_000_000_000)
    dt = datetime.fromtimestamp(secs, tz=timezone.utc)
    return dt.strftime("%Y-%m-%dT%H:%M:%S.") + f"{rem // 1_000_000:03d}Z"


def _base_quote(name: str) -> Tuple[str, str]:
    for quote in ("USDT", "USDC", "USD", "BTC", "ETH"):
        if name.endswith(quote) and len(name) > len(quote):
            return name[: -len(quote)], quote
    return "", ""


def _write_metadata(out_tape: Path, *,
                    exchange_name: str,
                    market: str,
                    symbol_id: int,
                    symbol_name: str,
                    first_ts_ns: int,
                    last_ts_ns: int,
                    trades_added: int) -> None:
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
        "recording_id": f"bybit-{market}-{symbol_name}",
        "description": f"Bybit {market} trades archive for {symbol_name}",
        "exchange": exchange_name,
        "exchange_type": "cex",
        "instrument_type": _INSTRUMENT_TYPE.get(market, ""),
        "connector_version": "bybit-archive-import",
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
        "custom": {"qty_unit": _QTY_UNIT.get(market, "")},
    }

    meta_path = out_tape / "metadata.json"
    if meta_path.exists():
        try:
            existing = json.loads(meta_path.read_text())
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


def _parse_file(csv_path: Path, market: str,
                spill_dir: Optional[Path] = None) -> _dedup.ParsedTrades:
    """Parse one archive file, nothing written yet. Raises
    ``UnknownLayoutError`` / ``NothingParsedError`` as ``trades_to_lrvx``
    documents."""
    seg = _market_path(market)
    src = _layout.CsvFile(csv_path, _LAYOUTS["spot" if seg == "spot" else "trading"],
                          source=f"Bybit {market} trade")
    rows = _dedup.TradeFile(_iter_rows(src), _dedup.trade_dtype(), sort=True,
                            spill_dir=spill_dir)
    try:
        src.check(rows.rows_read)
    except BaseException:
        rows.close()
        raise
    return _dedup.ParsedTrades(csv_path, rows, src.counts.invalid)


def _parse_range_file(job: Tuple[Path, str, Path]) -> _dedup.ParsedTrades:
    """One file of a range, parsed in a worker of ``_range.convert_days``.
    A cached file that does not open is evicted from the cache."""
    path, market, spill_dir = job
    with evict_if_corrupt(path):
        return _parse_file(path, market, spill_dir)


def _write_file(parsed: _dedup.ParsedTrades, out_tape: Path, *, append: bool,
                symbol_id: int, exchange_id: int, max_segment_mb: int,
                compression: str) -> ConvertStats:
    """Write a parsed file's rows that are not on the tape yet."""
    rows = parsed.rows
    try:
        done = _dedup.write_trade_file(
            rows, out_tape, append=append, symbol_id=symbol_id,
            exchange_id=exchange_id, max_segment_mb=max_segment_mb,
            compression=compression)
        return ConvertStats(
            trades_written=done.count,
            rows_read=rows.rows_read,
            rows_skipped=rows.rows_skipped,
            rows_invalid=parsed.rows_invalid,
            files_processed=1,
            first_ts_ns=done.first_ts_ns,
            last_ts_ns=done.last_ts_ns,
            last_trade_id=done.last_id,
        )
    finally:
        rows.close()


def _add_stats(agg: ConvertStats, s: ConvertStats) -> None:
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


def trades_to_lrvx(
    csv_path: Union[str, Path],
    out_tape: Union[str, Path],
    *,
    symbol_id: int = 1,
    symbol_name: str = "",
    market: str = "linear",
    exchange_id: int = 0,
    exchange_name: str = "bybit",
    append: bool = True,
    max_segment_mb: int = 256,
    compression: str = "none",
    write_metadata: bool = True,
) -> ConvertStats:
    """Convert one Bybit archive CSV (gzipped or extracted) into lrvx.

    ``market`` picks the layouts accepted: spot files for ``spot``,
    derivatives files for ``linear`` and ``inverse``. Raises
    ``UnknownLayoutError`` for any other header and
    ``NothingParsedError`` when the file has rows and none parse. A file
    whose trades are all on the tape already returns
    ``trades_written=0`` with ``rows_skipped`` set."""
    seg = _market_path(market)
    csv_path = Path(csv_path).expanduser()
    out_tape = Path(out_tape).expanduser()
    if not csv_path.exists():
        raise FileNotFoundError(csv_path)
    if not symbol_name:
        # `BTCUSDT2024-01-15.csv.gz` / `BTCUSDT_2024-01-15.csv.gz` → "BTCUSDT".
        stem = csv_path.name
        if stem.endswith(".csv.gz"):
            stem = stem[: -len(".csv.gz")]
        elif stem.endswith(".csv"):
            stem = stem[: -len(".csv")]
        symbol_name = re.sub(r"_?\d{4}-\d{2}-\d{2}$", "", stem)

    stats = _write_file(
        _parse_file(csv_path, market), out_tape, append=append,
        symbol_id=symbol_id, exchange_id=exchange_id,
        max_segment_mb=max_segment_mb, compression=compression)

    if write_metadata and (stats.trades_written or stats.rows_read):
        _write_metadata(
            out_tape, exchange_name=exchange_name, market=market,
            symbol_id=symbol_id, symbol_name=symbol_name,
            first_ts_ns=stats.first_ts_ns, last_ts_ns=stats.last_ts_ns,
            trades_added=stats.trades_written,
        )
    return stats


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
    exchange_name: str = "bybit",
    append: bool = True,
    max_segment_mb: int = 256,
    compression: str = "none",
    skip_missing: bool = False,
    workers: Optional[int] = None,
) -> ConvertStats:
    """Convert a range of UTC days (inclusive) into lrvx.

    Files are taken from, and missing ones downloaded into, the cache:
    ``<cache_root>/bybit`` (``~/.lrvx/archive-cache/bybit`` unless
    ``LRVX_ARCHIVE_CACHE`` says otherwise), or ``mirror`` when given,
    laid out as ``<market>/<symbol>/<file name as published>``.
    ``parallel`` downloads run at once.

    ``workers`` processes parse the files (default: one per core, at
    most one per file; ``1`` parses in this process); the tape is
    written here, a day at a time in date order, so it is the same tape
    whatever the count. An import that fails leaves the tape as it was.

    The symbol's shape and ``workers`` are checked before any request. A
    range in which no day is published raises ``NoFilesError``; a
    missing day raises ``FileNotFoundError`` unless ``skip_missing``."""
    _range.check_workers(workers)
    check_symbol(symbol, market)
    days = _range.days_between(date_from, date_to)
    if not days:
        return ConvertStats()
    mirror = _cache.exchange_dir("bybit", mirror)

    def _ensure(day: date) -> Path:
        local = _mirror_path(mirror, symbol, market, day)
        if not local.exists():
            _download(_archive_url(symbol, market, day), local)
        return local

    pairs = _range.fetch_days(days, _ensure, parallel=parallel,
                              skip_missing=skip_missing,
                              what=f"bybit {market} trades {symbol}")

    paths = [path for _, path in pairs if path is not None]
    out_tape = Path(out_tape).expanduser()
    agg = ConvertStats()

    def write(_: int, parsed: _dedup.ParsedTrades) -> None:
        _add_stats(agg, _write_file(
            parsed, out_tape, append=append, symbol_id=symbol_id,
            exchange_id=exchange_id, max_segment_mb=max_segment_mb,
            compression=compression))

    with _range.spill_dir() as spill, _range.all_or_nothing(out_tape):
        _range.convert_days([(path, market, spill) for path in paths],
                            _parse_range_file,
                            workers=_range.resolve_workers(workers, len(paths)),
                            then=write)
        _write_metadata(
            out_tape,
            exchange_name=exchange_name, market=market,
            symbol_id=symbol_id, symbol_name=symbol,
            first_ts_ns=agg.first_ts_ns, last_ts_ns=agg.last_ts_ns,
            trades_added=agg.trades_written,
        )

    return agg


UnknownLayoutError = _layout.UnknownLayoutError
NothingParsedError = _layout.NothingParsedError

__all__ = [
    "ConvertStats",
    "NothingParsedError",
    "check_symbol",
    "UnknownLayoutError",
    "trades_to_lrvx",
    "range_to_lrvx",
]
