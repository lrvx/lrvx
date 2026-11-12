"""Bitget public archive → lrvx converter.

Bitget publishes daily trade files behind its "History data download"
page (https://www.bitget.com/data-download, "Transaction history",
daily). The files sit on its CDN at

    https://img.bitgetimg.com/online/trades/<CODE>/<SYMBOL>/<NAME>.zip

where ``<CODE>`` is Bitget's product code for the market:

  * ``spot``  → ``SPBL``   spot pairs (``BTCUSDT``)
  * ``umcbl`` → ``UMCBL``  USDT-margined perpetuals (``BTCUSDT``)
  * ``cmcbl`` → ``CMCBL``  USDC-margined perpetuals (``BTCPERP``)
  * ``dmcbl`` → ``DMCBL``  coin-margined perpetuals (``BTCUSD``)

A day is split into numbered parts of at most 100,000 trades,
``_001``, ``_002``, ... The part name has changed over time; a day uses
exactly one of

    <SYMBOL>_<CODE>_<YYYYMMDD>_<NNN>     (older days)
    <YYYYMMDD>_<NNN>                     (later days)
    <SYMBOL>_<YYYYMMDD>_<NNN>            (recent days)

The importer tries them in that order and downloads parts until the
next one is missing (the CDN answers 403 for a missing file). These
names are what the download page hands out; Bitget does not document
them as an API, so a future renaming shows up as a day that cannot be
found. A day is a UTC+8 calendar day, as the download page states: the
file for 2024-01-15 holds trades from 2024-01-14T16:00Z to
2024-01-15T16:00Z, and ``date_from`` / ``date_to`` name such days.

Symbols carry no product suffix: ``BTCUSDT`` for spot and umcbl,
``BTCPERP`` for cmcbl, ``BTCUSD`` for dmcbl. The range form caches the
parts under ``<cache_root>/bitget/<market>/<symbol>/`` (``mirror``
replaces ``<cache_root>/bitget``), with the download policy of
``_download``.

The CSV inside each part is

    trade_id,timestamp,price,side,volume(quote),size(base)

``timestamp`` is Unix milliseconds (recent files carry whole seconds).
``side`` is the taker side, ``buy`` / ``sell``. ``trade_id`` is a
19-digit integer, used as the lrvx ``trade_id`` for append dedup.
``size(base)`` is the quantity in the base coin for every market,
coin-margined included, and is what the tape stores (the live Bitget
connector also reports trade size in the base coin). ``volume(quote)``
is the quote notional and is not stored. Any other header is rejected
with ``UnknownLayoutError``.
"""
from __future__ import annotations

import json
import os
from dataclasses import dataclass
from datetime import date, datetime, timezone
from pathlib import Path
from typing import Iterator, List, Optional, Tuple, Union

from . import _cache, _dedup, _layout, _range
from ._download import NotPublished, evict_if_corrupt
from ._download import fetch as _download


_BASE_URL = "https://img.bitgetimg.com/online/trades"

_CODES = {
    "spot":  "SPBL",
    "umcbl": "UMCBL",
    "cmcbl": "CMCBL",
    "dmcbl": "DMCBL",
}
_MARKETS = set(_CODES)

_INSTRUMENT_TYPE = {
    "spot":  "spot",
    "umcbl": "perpetual",
    "cmcbl": "perpetual",
    "dmcbl": "perpetual",
}

_PART_NAMES = (
    "{symbol}_{code}_{day}_{part:03d}",
    "{day}_{part:03d}",
    "{symbol}_{day}_{part:03d}",
)

# Parts per day are numbered from 001; a day with more than this many is
# not something the archive has produced.
_MAX_PARTS = 999

_LAYOUTS = (
    _layout.Layout("trades", ("trade_id", "timestamp", "price", "side",
                              "volume(quote)", "size(base)")),
)

DateLike = Union[str, date]

# The archive's days start at midnight UTC+8.
DAY_UTC_OFFSET_HOURS = 8

# Symbol suffix and example per market.
_SYMBOL_FORM = {
    "spot":  ("", "BTCUSDT"),
    "umcbl": ("USDT", "BTCUSDT"),
    "cmcbl": ("PERP", "BTCPERP"),
    "dmcbl": ("USD", "BTCUSD"),
}


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


def _code(market: str) -> str:
    if market not in _CODES:
        raise ValueError(
            f"unknown market '{market}'. expected one of {sorted(_MARKETS)}"
        )
    return _CODES[market]


def check_symbol(symbol: str, market: str) -> None:
    """Raise ValueError unless ``symbol`` has the shape of a Bitget
    ``market`` symbol as the archive names it. A local check; no
    request is made."""
    _code(market)
    suffix, example = _SYMBOL_FORM[market]
    ok = (_range.is_upper_alnum(symbol) and symbol.endswith(suffix)
          and len(symbol) > len(suffix))
    if not ok:
        if "_" in symbol:
            expected = "the symbol without a product suffix such as _UMCBL"
        elif suffix:
            expected = f"<COIN>{suffix} in upper case"
        else:
            expected = "the pair in upper case with no separator"
        raise _range.bad_symbol("Bitget", market, symbol, expected, example)


def _part_url(symbol: str, market: str, day: date, pattern: str,
              part: int) -> str:
    code = _code(market)
    name = pattern.format(symbol=symbol, code=code,
                          day=day.strftime("%Y%m%d"), part=part)
    return f"{_BASE_URL}/{code}/{symbol}/{name}.zip"


def _archive_url(symbol: str, market: str, day: date) -> str:
    """URL of the first part of ``day`` in the newest naming."""
    return _part_url(symbol, market, day, _PART_NAMES[-1], 1)


def _mirror_dir(mirror: Path, symbol: str, market: str) -> Path:
    return mirror / market / symbol


def _mirror_path(mirror: Path, symbol: str, market: str, day: date,
                 part: int = 1) -> Path:
    """Where part ``part`` of ``day`` sits in the mirror, named as in the
    first part naming."""
    url = _part_url(symbol, market, day, _PART_NAMES[0], part)
    return _mirror_dir(mirror, symbol, market) / url.rsplit("/", 1)[1]


def _manifest_path(mirror: Path, symbol: str, market: str, day: date) -> Path:
    return _mirror_dir(mirror, symbol, market) / f"{symbol}-{day.isoformat()}.parts"


def _fetch(url: str, dest: Path) -> bool:
    """Download ``url`` to ``dest``. False when the archive does not
    publish the file; raises on any other failure."""
    try:
        _download(url, dest)
    except NotPublished:
        return False
    return True


def _download_day(mirror: Path, symbol: str, market: str,
                  day: date) -> List[Path]:
    """Every part of ``day`` in the mirror, downloading what is missing.
    The part list is kept in a manifest once complete. Raises
    NotPublished, naming the first URL tried, when the archive has no
    file for the day under any part naming."""
    manifest = _manifest_path(mirror, symbol, market, day)
    folder = _mirror_dir(mirror, symbol, market)
    if manifest.exists():
        paths = [folder / n for n in manifest.read_text().split()]
        if paths and all(p.exists() for p in paths):
            return paths
    for pattern in _PART_NAMES:
        paths: List[Path] = []
        for part in range(1, _MAX_PARTS + 1):
            url = _part_url(symbol, market, day, pattern, part)
            dest = folder / url.rsplit("/", 1)[1]
            if not dest.exists() and not _fetch(url, dest):
                break
            paths.append(dest)
        if paths:
            tmp = manifest.with_name(f".{manifest.name}.{os.getpid()}.part")
            tmp.write_text("\n".join(p.name for p in paths) + "\n")
            os.replace(tmp, manifest)
            return paths
    first = _part_url(symbol, market, day, _PART_NAMES[0], 1)
    raise NotPublished(
        first, None,
        f"Bitget archive has no {market} trade file for {symbol} on "
        f"{day.isoformat()} (tried {first} and the other part namings)")


def _iter_rows(src: _layout.CsvFile) -> Iterator[Tuple[int, float, float, int, int]]:
    """Yield ``(trade_id, price, qty, ts_ms, side)`` rows of a Bitget
    trade file."""
    for row in src.rows():
        try:
            side = _layout.side_of(row[3])
            if side is None:
                raise ValueError(row[3])
            yield (int(row[0]), float(row[2]), float(row[5]), int(row[1]), side)
        except (ValueError, IndexError):
            src.counts.bad(row)


def _ns_to_iso(ns: int) -> str:
    if ns <= 0:
        return ""
    secs, rem = divmod(int(ns), 1_000_000_000)
    dt = datetime.fromtimestamp(secs, tz=timezone.utc)
    return dt.strftime("%Y-%m-%dT%H:%M:%S.") + f"{rem // 1_000_000:03d}Z"


def _base_quote(name: str) -> Tuple[str, str]:
    if name.endswith("PERP") and len(name) > 4:
        return name[:-4], "USDC"
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
        "recording_id": f"bitget-{market}-{symbol_name}",
        "description": f"Bitget {market} trades archive for {symbol_name}",
        "exchange": exchange_name,
        "exchange_type": "cex",
        "instrument_type": _INSTRUMENT_TYPE.get(market, ""),
        "connector_version": "bitget-archive-import",
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
        "custom": {"qty_unit": "base"},
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
    """``BTCUSDT_UMCBL_20240115_001.zip`` / ``BTCUSDT_20261109_001.zip`` →
    ``BTCUSDT``; empty for ``20250115_001.zip``, which names no symbol."""
    stem = path.name
    for ext in (".csv.gz", ".csv", ".zip"):
        if stem.endswith(ext):
            stem = stem[: -len(ext)]
            break
    head = stem.split("_", 1)[0]
    return "" if head.isdigit() else head


def trades_to_lrvx(
    csv_path: Union[str, Path],
    out_tape: Union[str, Path],
    *,
    symbol_id: int = 1,
    symbol_name: str = "",
    market: str = "umcbl",
    exchange_id: int = 0,
    exchange_name: str = "bitget",
    append: bool = True,
    max_segment_mb: int = 256,
    compression: str = "none",
    write_metadata: bool = True,
) -> ConvertStats:
    """Convert one Bitget archive part (zipped, gzipped, or extracted)
    into lrvx.

    ``symbol_name`` defaults to the symbol in the file name; parts named
    ``<YYYYMMDD>_<NNN>.zip`` carry none, so pass it. Raises
    ``UnknownLayoutError`` for a header Bitget does not publish and
    ``NothingParsedError`` when the file has rows and none parse. A part
    whose trades are all on the tape already returns
    ``trades_written=0`` with ``rows_skipped`` set."""
    _code(market)
    csv_path = Path(csv_path).expanduser()
    out_tape = Path(out_tape).expanduser()
    if not csv_path.exists():
        raise FileNotFoundError(csv_path)
    if not symbol_name:
        symbol_name = _symbol_from_file(csv_path)
        if not symbol_name:
            raise ValueError(
                f"{csv_path.name} does not name its symbol; pass symbol_name")

    src = _layout.CsvFile(csv_path, _LAYOUTS, source="Bitget trade")
    with _dedup.TradeFile(_iter_rows(src), _dedup.trade_dtype(),
                          sort=True) as rows:
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
            files_processed=1,
            first_ts_ns=done.first_ts_ns,
            last_ts_ns=done.last_ts_ns,
            last_trade_id=done.last_id if done.count else rows.id_max,
        )

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
    exchange_name: str = "bitget",
    append: bool = True,
    max_segment_mb: int = 256,
    compression: str = "none",
    skip_missing: bool = False,
) -> ConvertStats:
    """Convert a range of Bitget days (UTC+8 dates, inclusive), every
    part of each, into lrvx: ``date_from="2024-01-15"`` starts at
    2024-01-14T16:00Z.

    Parts are taken from, and missing ones downloaded into, the cache:
    ``<cache_root>/bitget`` (``~/.lrvx/archive-cache/bitget`` unless
    ``LRVX_ARCHIVE_CACHE`` says otherwise), or ``mirror`` when given,
    laid out as ``<market>/<symbol>/<part name>``.

    The symbol's shape is checked before any request. A range in which
    no day is published raises ``NoFilesError``; a missing day raises
    ``FileNotFoundError`` unless ``skip_missing``."""
    check_symbol(symbol, market)
    days = _range.days_between(date_from, date_to)
    if not days:
        return ConvertStats()
    mirror = _cache.exchange_dir("bitget", mirror)

    pairs = _range.fetch_days(
        days, lambda day: _download_day(mirror, symbol, market, day),
        parallel=parallel, skip_missing=skip_missing,
        what=f"bitget {market} trades {symbol}")

    agg = ConvertStats()
    for day, paths in pairs:
        for path in paths or ():
            with evict_if_corrupt(path):
                s = trades_to_lrvx(
                    path, out_tape,
                    symbol_id=symbol_id, symbol_name=symbol, market=market,
                    exchange_id=exchange_id, exchange_name=exchange_name,
                    append=append, max_segment_mb=max_segment_mb,
                    compression=compression, write_metadata=False,
                )
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
        Path(out_tape).expanduser(),
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
