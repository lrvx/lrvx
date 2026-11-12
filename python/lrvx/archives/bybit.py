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
symbol naming (``BTCUSDT`` linear, ``BTCUSD`` inverse). The symbol is
used verbatim.

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

import hashlib
import json
import re
import shutil
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from datetime import date, datetime, timedelta, timezone
from pathlib import Path
from typing import Iterator, List, Optional, Tuple, Union

from . import _cache, _dedup, _layout


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


def _download(url: str, dest: Path, *, retries: int = 3,
              backoff: float = 2.0) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_suffix(dest.suffix + ".tmp")
    last_err: Optional[BaseException] = None
    for attempt in range(retries):
        try:
            with urllib.request.urlopen(url, timeout=60) as resp, open(tmp, "wb") as f:
                shutil.copyfileobj(resp, f)
            tmp.replace(dest)
            return
        except (urllib.error.URLError, OSError, TimeoutError) as e:
            last_err = e
            if tmp.exists():
                try:
                    tmp.unlink()
                except OSError:
                    pass
            if attempt < retries - 1:
                time.sleep(backoff * (2 ** attempt))
    raise RuntimeError(f"failed to download {url}: {last_err}") from last_err


def _hash_trade_id(token: str) -> int:
    """Bybit's `trdMatchID` is a UUID string. Fold it to an int64 via
    the low 8 bytes of MD5 so the lrvx ``trade_id`` field (uint64)
    can carry it. The folded ids are unordered, so append dedup checks
    membership, never ``<= max``. Collisions are vanishingly rare
    within a single symbol's daily volume; one would only matter when
    a new trade collides with a trade already on the tape in the same
    time window, which skips the new trade."""
    try:
        return int(token)
    except (ValueError, TypeError):
        pass
    h = hashlib.md5(token.encode("ascii", errors="ignore")).digest()
    return int.from_bytes(h[:8], "big", signed=False)


def _parse_ts_ns(token: str) -> int:
    """Parse a Bybit timestamp string (Unix seconds, dot-decimal) into
    nanoseconds without losing precision to float round-off. Accepts
    ``1700000001.25`` as well as ``1700000001.250000000``."""
    s = token.strip()
    if "." in s:
        secs_str, frac_str = s.split(".", 1)
        # Pad / truncate the fractional part to 9 digits (ns).
        if len(frac_str) > 9:
            frac_str = frac_str[:9]
        else:
            frac_str = frac_str.ljust(9, "0")
        return int(secs_str) * 1_000_000_000 + int(frac_str)
    return int(s) * 1_000_000_000


def _iter_rows(src: _layout.CsvFile) -> Iterator[Tuple[int, float, float, int, int]]:
    """Yield ``(trade_id, price, qty, ts_ns, side)`` rows from a Bybit
    archive CSV, spot or derivatives, by its detected layout."""
    for row in src.rows():
        try:
            if src.layout.columns[0] == "id":
                side = _layout.side_of(row[4])
                if side is None:
                    raise ValueError(row[4])
                yield (int(row[0]), float(row[2]), float(row[3]),
                       int(row[1]) * 1_000_000, side)
            else:
                side = _layout.side_of(row[2])
                if side is None or not row[6]:
                    raise ValueError(row[2])
                yield (_hash_trade_id(row[6]), float(row[4]), float(row[3]),
                       _parse_ts_ns(row[0]), side)
        except (ValueError, IndexError):
            src.counts.bad(row)


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

    src = _layout.CsvFile(csv_path, _LAYOUTS["spot" if seg == "spot" else "trading"],
                          source=f"Bybit {market} trade")
    with _dedup.TradeFile(_iter_rows(src), _dedup.trade_dtype(),
                          sort=True) as rows:
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
            last_trade_id=done.last_id,
        )

    if write_metadata and (stats.trades_written or stats.rows_read):
        _write_metadata(
            out_tape, exchange_name=exchange_name, market=market,
            symbol_id=symbol_id, symbol_name=symbol_name,
            first_ts_ns=stats.first_ts_ns, last_ts_ns=stats.last_ts_ns,
            trades_added=stats.trades_written,
        )
    return stats


def _coerce_day(d: DateLike) -> date:
    return d if isinstance(d, date) else date.fromisoformat(d)


def _iter_days(date_from: DateLike, date_to: DateLike) -> Iterator[date]:
    cur = _coerce_day(date_from)
    end = _coerce_day(date_to)
    while cur <= end:
        yield cur
        cur += timedelta(days=1)


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
) -> ConvertStats:
    """Convert a multi-day Bybit range into lrvx.

    When ``mirror`` is omitted, the shared ``cache_root() / "bybit"``
    directory is used so repeated calls reuse the same on-disk cache
    across runs. Pass an explicit path to keep the downloaded files
    in a per-project folder."""
    days = list(_iter_days(date_from, date_to))
    if not days:
        return ConvertStats()

    if mirror is None:
        mirror = _cache.cache_root() / "bybit"
    mirror = Path(mirror).expanduser()
    mirror.mkdir(parents=True, exist_ok=True)

    def _ensure(day: date) -> Tuple[date, Optional[Path]]:
        local = _mirror_path(mirror, symbol, market, day)
        if local.exists():
            return day, local
        try:
            _download(_archive_url(symbol, market, day), local)
        except Exception:
            if skip_missing:
                return day, None
            raise
        return day, local

    pairs: List[Tuple[date, Optional[Path]]] = []
    with ThreadPoolExecutor(max_workers=max(1, int(parallel))) as ex:
        for pair in ex.map(_ensure, days):
            pairs.append(pair)

    agg = ConvertStats()
    for day, path in pairs:
        if path is None:
            continue
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
        Path(out_tape),
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
    "UnknownLayoutError",
    "trades_to_lrvx",
    "range_to_lrvx",
]
