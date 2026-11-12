"""OKX public archive → lrvx converter.

OKX publishes daily trade files on its CDN and lists them through the
public ``GET /api/v5/public/market-data-history`` endpoint (module 1,
tick-by-tick trades). A file's URL is

    https://static.okx.com/cdn/okex/traderecords/trades/daily/<YYYYMMDD>/<FILE>.zip

with ``<FILE>`` by market:

  * spot:    ``<INST>-trades-<YYYY-MM-DD>``       (``BTC-USDT``)
  * swap:    ``<INST>-trades-<YYYY-MM-DD>``       (``BTC-USDT-SWAP``)
  * futures: ``<FAMILY>-futureschain-trades-<YYYY-MM-DD>``, every
    expiry of the family in one file (``BTC-USD`` for ``BTC-USD-240329``)
  * option:  ``<FAMILY>-optionchain-trades-<YYYY-MM-DD>``, every
    option of the family in one file (``BTC-USD`` for
    ``BTC-USD-240329-50000-C``)

A day is a UTC+8 calendar day: the file for 2024-01-15 holds trades
from 2024-01-14T16:00Z to 2024-01-15T16:00Z, and ``date_from`` /
``date_to`` name such days. Files usually appear two days after the day
ends.

The range form caches the downloaded files under
``<cache_root>/okx/<market>/<instrument>/`` (the family, ``BTC-USD``,
for futures and options chain files; ``mirror`` replaces
``<cache_root>/okx``), with the download policy of ``_download``.

The CSV layout is read from the header row. OKX publishes:

    instrument_name,trade_id,side,price,size,created_time
    instrument_name,trade_id,side,price,size,created_time,source
    trade_id,side,size,price,created_time      (legacy per-instrument
                                                files; each header cell
                                                carries a Chinese
                                                translation after "/")

``created_time`` is Unix milliseconds. ``side`` is the taker side,
``buy`` / ``sell`` in any case. ``trade_id`` is an integer, increasing
per instrument, used as the lrvx ``trade_id`` for append dedup. Any
other header is rejected with ``UnknownLayoutError``.

Quantity: ``size`` is stored as published, so it is in the base
currency for spot and in contracts for swap, futures and option
(``BTC-USDT-SWAP``: 0.01 BTC per contract; ``BTC-USD-SWAP`` and the
BTC-USD futures: 100 USD per contract). The live path through the ccxt
bridge reports contract markets in contracts too. ``qty_multiplier``
scales every size by a constant, e.g. 0.01 to store ``BTC-USDT-SWAP``
in BTC; the importer does not know contract values, so the caller
supplies it (OKX's ``ctVal`` / ``ctMult`` from
``/api/v5/public/instruments``). ``metadata.json`` records the unit
under ``custom.qty_unit`` and the multiplier under
``custom.qty_multiplier``.

One tape holds one instrument. Importing a futures or option instrument
reads the family's chain file and keeps the rows of that instrument;
the other rows are counted in ``rows_other_instruments``.
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


_BASE_URL = "https://static.okx.com/cdn/okex/traderecords/trades/daily"

_MARKETS = {"spot", "swap", "futures", "option"}

_INSTRUMENT_TYPE = {
    "spot":    "spot",
    "swap":    "perpetual",
    "futures": "futures",
    "option":  "option",
}

_CHAIN = {"futures": "futureschain", "option": "optionchain"}

_LAYOUTS = (
    _layout.Layout("instrument", ("instrument_name", "trade_id", "side",
                                  "price", "size", "created_time")),
    _layout.Layout("instrument+source", ("instrument_name", "trade_id", "side",
                                         "price", "size", "created_time",
                                         "source")),
    _layout.Layout("legacy", ("trade_id", "side", "size", "price",
                              "created_time")),
)

DateLike = Union[str, date]

# The archive's days start at midnight UTC+8.
DAY_UTC_OFFSET_HOURS = 8


class InstrumentNotInFileError(ValueError):
    """The file parsed but holds no trade of the requested instrument."""


@dataclass
class ConvertStats:
    trades_written: int = 0
    rows_read: int = 0
    rows_skipped: int = 0
    rows_invalid: int = 0
    rows_other_instruments: int = 0
    files_processed: int = 0
    first_ts_ns: int = 0
    last_ts_ns: int = 0
    last_trade_id: int = 0


_SHAPES = {
    "spot": ("<BASE>-<QUOTE>", "BTC-USDT"),
    "swap": ("<BASE>-<QUOTE>-SWAP", "BTC-USDT-SWAP"),
    "futures": ("<BASE>-<QUOTE>-<YYMMDD>", "BTC-USD-240329"),
    "option": ("<BASE>-<QUOTE>-<YYMMDD>-<STRIKE>-<C or P>",
               "BTC-USD-240329-50000-C"),
}


def check_symbol(symbol: str, market: str) -> None:
    """Raise ValueError unless ``symbol`` has the shape of an OKX
    ``market`` instrument id. A local check; no request is made."""
    if market not in _MARKETS:
        raise ValueError(
            f"unknown market '{market}'. expected one of {sorted(_MARKETS)}"
        )
    parts = symbol.split("-")
    # The quote may carry a margin tag, as in the family BTC-USD_UM.
    pair = (len(parts) >= 2 and _range.is_upper_alnum(parts[0])
            and all(_range.is_upper_alnum(t) for t in parts[1].split("_")))
    expiry = len(parts) >= 3 and parts[2].isdigit() and len(parts[2]) == 6
    ok = pair and {
        "spot": len(parts) == 2,
        "swap": len(parts) == 3 and parts[2] == "SWAP",
        "futures": len(parts) == 3 and expiry,
        "option": (len(parts) == 5 and expiry
                   and parts[3].replace(".", "", 1).isdigit()
                   and parts[4] in ("C", "P")),
    }[market]
    if not ok:
        expected, example = _SHAPES[market]
        raise _range.bad_symbol("OKX", market, symbol,
                                f"an instrument id {expected}", example)


def _url_symbol(symbol: str, market: str) -> str:
    """The instrument the file is named after: the family for the
    futures and option chain files."""
    return "-".join(symbol.split("-")[:2]) if market in _CHAIN else symbol


def _archive_name(symbol: str, market: str, day: date) -> str:
    check_symbol(symbol, market)
    iso = day.isoformat()
    if market in _CHAIN:
        return f"{_url_symbol(symbol, market)}-{_CHAIN[market]}-trades-{iso}.zip"
    return f"{symbol}-trades-{iso}.zip"


def _archive_url(symbol: str, market: str, day: date) -> str:
    name = _archive_name(symbol, market, day)
    return f"{_BASE_URL}/{day.strftime('%Y%m%d')}/{name}"


def _mirror_path(mirror: Path, symbol: str, market: str, day: date) -> Path:
    return (mirror / market / _url_symbol(symbol, market)
            / _archive_name(symbol, market, day))


class _Other:
    def __init__(self) -> None:
        self.rows = 0
        self.names: List[str] = []


def _iter_rows(src: _layout.CsvFile, symbol: str, qty_multiplier: float,
               other: _Other) -> Iterator[Tuple[int, float, float, int, int]]:
    """Yield ``(trade_id, price, qty, ts_ms, side)`` for the rows of
    ``symbol`` (every row when the layout has no instrument column)."""
    inst = tid = side_i = price_i = size_i = ts_i = None
    for row in src.rows():
        if tid is None:
            lay = src.layout
            inst = (lay.index("instrument_name")
                    if "instrument_name" in lay.columns else None)
            tid, side_i = lay.index("trade_id"), lay.index("side")
            price_i, size_i = lay.index("price"), lay.index("size")
            ts_i = lay.index("created_time")
        if (inst is not None and symbol and len(row) == len(src.layout.columns)
                and row[inst] != symbol):
            other.rows += 1
            if len(other.names) < 5 and row[inst] not in other.names:
                other.names.append(row[inst])
            continue
        try:
            side = _layout.side_of(row[side_i])
            if side is None:
                raise ValueError(row[side_i])
            yield (int(row[tid]), float(row[price_i]),
                   float(row[size_i]) * qty_multiplier, int(row[ts_i]), side)
        except (ValueError, IndexError):
            src.counts.bad(row)


def _ns_to_iso(ns: int) -> str:
    if ns <= 0:
        return ""
    secs, rem = divmod(int(ns), 1_000_000_000)
    dt = datetime.fromtimestamp(secs, tz=timezone.utc)
    return dt.strftime("%Y-%m-%dT%H:%M:%S.") + f"{rem // 1_000_000:03d}Z"


def _base_quote(name: str) -> Tuple[str, str]:
    """Pull (base, quote) out of an OKX instrument id: the first two
    hyphen-separated tokens."""
    parts = name.split("-")
    if len(parts) >= 2:
        return parts[0], parts[1]
    return "", ""


def _qty_custom(market: str, qty_multiplier: float) -> dict:
    return {"qty_unit": "base" if market == "spot" else "contracts",
            "qty_multiplier": float(qty_multiplier)}


def _write_metadata(out_tape: Path, *,
                    exchange_name: str,
                    market: str,
                    symbol_id: int,
                    symbol_name: str,
                    first_ts_ns: int,
                    last_ts_ns: int,
                    trades_added: int,
                    qty_multiplier: float = 1.0) -> None:
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
        "recording_id": f"okx-{market}-{symbol_name}",
        "description": f"OKX {market} trades archive for {symbol_name}",
        "exchange": exchange_name,
        "exchange_type": "cex",
        "instrument_type": _INSTRUMENT_TYPE.get(market, ""),
        "connector_version": "okx-archive-import",
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
        "custom": _qty_custom(market, qty_multiplier),
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


def _symbol_from_file(path: Path) -> str:
    """``BTC-USDT-SWAP-trades-2024-01-15.zip`` → ``BTC-USDT-SWAP``; empty
    for a chain file, whose name does not identify one instrument."""
    stem = path.name
    for ext in (".csv.gz", ".csv", ".zip"):
        if stem.endswith(ext):
            stem = stem[: -len(ext)]
            break
    for chain in _CHAIN.values():
        if f"-{chain}-" in stem:
            return ""
    idx = stem.find("-trades-")
    return stem[:idx] if idx > 0 else stem


def _convert_file(csv_path: Path, out_tape: Path, *, symbol_id: int,
                  symbol_name: str, market: str, exchange_id: int,
                  exchange_name: str, append: bool, max_segment_mb: int,
                  compression: str, write_metadata: bool,
                  qty_multiplier: float) -> ConvertStats:
    src = _layout.CsvFile(csv_path, _LAYOUTS, source="OKX trade")
    other = _Other()
    with _dedup.TradeFile(_iter_rows(src, symbol_name, qty_multiplier, other),
                          _dedup.trade_dtype(), sort=True) as rows:
        src.check(rows.rows_read)
        done = _dedup.write_trade_file(
            rows, out_tape, append=append, symbol_id=symbol_id,
            exchange_id=exchange_id, max_segment_mb=max_segment_mb,
            compression=compression, ts_ns_per_unit=1_000_000)
        stats = ConvertStats(
            trades_written=done.count,
            rows_read=rows.rows_read,
            rows_skipped=rows.rows_skipped,
            rows_invalid=src.counts.invalid,
            rows_other_instruments=other.rows,
            files_processed=1,
            first_ts_ns=done.first_ts_ns,
            last_ts_ns=done.last_ts_ns,
            last_trade_id=done.last_id if done.count else rows.id_max,
        )
    if other.rows and not stats.rows_read and not stats.rows_invalid:
        raise InstrumentNotInFileError(
            f"{csv_path}: no trade of '{symbol_name}' among {other.rows} rows; "
            f"the file holds {', '.join(other.names)}"
            + (", ..." if len(other.names) == 5 else ""))
    if write_metadata and (stats.trades_written or stats.rows_read):
        _write_metadata(
            out_tape, exchange_name=exchange_name, market=market,
            symbol_id=symbol_id, symbol_name=symbol_name,
            first_ts_ns=stats.first_ts_ns, last_ts_ns=stats.last_ts_ns,
            trades_added=stats.trades_written, qty_multiplier=qty_multiplier,
        )
    return stats


def trades_to_lrvx(
    csv_path: Union[str, Path],
    out_tape: Union[str, Path],
    *,
    symbol_id: int = 1,
    symbol_name: str = "",
    market: str = "swap",
    exchange_id: int = 0,
    exchange_name: str = "okx",
    append: bool = True,
    max_segment_mb: int = 256,
    compression: str = "none",
    write_metadata: bool = True,
    qty_multiplier: float = 1.0,
) -> ConvertStats:
    """Convert one OKX archive file (zipped, gzipped, or extracted) into
    lrvx.

    ``symbol_name`` defaults to the instrument in the file name; it is
    required for a futures or option chain file, whose rows are filtered
    to that instrument. Raises ``UnknownLayoutError`` for a header OKX
    does not publish, ``NothingParsedError`` when the file has rows and
    none parse, and ``InstrumentNotInFileError`` when a chain file holds
    no trade of ``symbol_name``. A file whose trades are all on the tape
    already returns ``trades_written=0`` with ``rows_skipped`` set."""
    csv_path = Path(csv_path).expanduser()
    out_tape = Path(out_tape).expanduser()
    if not csv_path.exists():
        raise FileNotFoundError(csv_path)
    if market not in _MARKETS:
        raise ValueError(
            f"unknown market '{market}'. expected one of {sorted(_MARKETS)}")
    if not symbol_name:
        symbol_name = _symbol_from_file(csv_path)
        if not symbol_name:
            raise ValueError(
                f"{csv_path.name} is a chain file holding many instruments; "
                f"pass symbol_name to pick one")
    return _convert_file(
        csv_path, out_tape, symbol_id=symbol_id, symbol_name=symbol_name,
        market=market, exchange_id=exchange_id, exchange_name=exchange_name,
        append=append, max_segment_mb=max_segment_mb, compression=compression,
        write_metadata=write_metadata, qty_multiplier=qty_multiplier)


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
    exchange_name: str = "okx",
    append: bool = True,
    max_segment_mb: int = 256,
    compression: str = "none",
    skip_missing: bool = False,
    qty_multiplier: float = 1.0,
) -> ConvertStats:
    """Convert a range of OKX days (UTC+8 dates, inclusive) into lrvx:
    ``date_from="2024-01-15"`` starts at 2024-01-14T16:00Z.

    Files are taken from, and missing ones downloaded into, the cache:
    ``<cache_root>/okx`` (``~/.lrvx/archive-cache/okx`` unless
    ``LRVX_ARCHIVE_CACHE`` says otherwise), or ``mirror`` when given,
    laid out as ``<market>/<instrument or family>/<file name as
    published>``.

    The instrument id's shape is checked before any request. A range in
    which no day is published raises ``NoFilesError``; a missing day
    raises ``FileNotFoundError`` unless ``skip_missing``. For futures
    and options, a day whose chain file holds no trade of ``symbol``
    adds nothing; the range raises ``InstrumentNotInFileError`` only
    when no day held one."""
    check_symbol(symbol, market)
    days = _range.days_between(date_from, date_to)
    if not days:
        return ConvertStats()
    mirror = _cache.exchange_dir("okx", mirror)

    def _ensure(day: date) -> Path:
        local = _mirror_path(mirror, symbol, market, day)
        if not local.exists():
            _download(_archive_url(symbol, market, day), local)
        return local

    pairs = _range.fetch_days(days, _ensure, parallel=parallel,
                              skip_missing=skip_missing,
                              what=f"okx {market} trades {symbol}")

    agg = ConvertStats()
    absent: List[str] = []
    for day, path in pairs:
        if path is None:
            continue
        try:
            with evict_if_corrupt(path):
                s = _convert_file(
                    path, Path(out_tape).expanduser(),
                    symbol_id=symbol_id, symbol_name=symbol, market=market,
                    exchange_id=exchange_id, exchange_name=exchange_name,
                    append=append, max_segment_mb=max_segment_mb,
                    compression=compression, write_metadata=False,
                    qty_multiplier=qty_multiplier,
                )
        except InstrumentNotInFileError as e:
            absent.append(str(e))
            agg.files_processed += 1
            continue
        agg.trades_written += s.trades_written
        agg.rows_read += s.rows_read
        agg.rows_skipped += s.rows_skipped
        agg.rows_invalid += s.rows_invalid
        agg.rows_other_instruments += s.rows_other_instruments
        agg.files_processed += s.files_processed
        if s.first_ts_ns and (agg.first_ts_ns == 0 or s.first_ts_ns < agg.first_ts_ns):
            agg.first_ts_ns = s.first_ts_ns
        if s.last_ts_ns > agg.last_ts_ns:
            agg.last_ts_ns = s.last_ts_ns
        if s.last_trade_id > agg.last_trade_id:
            agg.last_trade_id = s.last_trade_id

    if absent and not agg.rows_read:
        raise InstrumentNotInFileError(
            f"no day in the range holds a trade of '{symbol}': " + absent[0])

    _write_metadata(
        Path(out_tape).expanduser(),
        exchange_name=exchange_name, market=market,
        symbol_id=symbol_id, symbol_name=symbol,
        first_ts_ns=agg.first_ts_ns, last_ts_ns=agg.last_ts_ns,
        trades_added=agg.trades_written, qty_multiplier=qty_multiplier,
    )

    return agg


UnknownLayoutError = _layout.UnknownLayoutError
NothingParsedError = _layout.NothingParsedError

__all__ = [
    "ConvertStats",
    "InstrumentNotInFileError",
    "NothingParsedError",
    "check_symbol",
    "UnknownLayoutError",
    "trades_to_lrvx",
    "range_to_lrvx",
]
