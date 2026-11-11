"""Bitget public archive → lrvx converter.

Bitget publishes daily trade ticks on their S3 / CDN mirror. The
on-disk CSV layout is:

    trade_id, price, size, side, timestamp_ms

`trade_id` is an integer exchange-assigned id, used directly as the
lrvx ``trade_id`` for append-safe dedup. `side` is the active flow
as ``buy`` / ``sell`` lowercase strings; the importer maps to
lrvx's ``Side::BUY`` / ``Side::SELL``. `timestamp_ms` is Unix
milliseconds.

Markets use Bitget's native codes:

  * ``spot``   — spot pairs (``BTCUSDT``)
  * ``umcbl``  — USDT-margined perpetuals (``BTCUSDT``)
  * ``cmcbl``  — coin-margined perpetuals (``BTCUSD``)

The codes are awkward but they are what Bitget's own API uses, so
the importer keeps them verbatim. Cross-exchange normalisation is a
connectors concern.

URL layout per market on the public archive:

  * spot:   ``<BASE>/spot/<SYMBOL>/<SYMBOL>-trades-<YYYY-MM-DD>.zip``
  * umcbl:  ``<BASE>/umcbl/<SYMBOL>/<SYMBOL>-trades-<YYYY-MM-DD>.zip``
  * cmcbl:  ``<BASE>/cmcbl/<SYMBOL>/<SYMBOL>-trades-<YYYY-MM-DD>.zip``

Both ``.zip`` and ``.csv.gz`` packaging are accepted on disk.
"""
from __future__ import annotations

import csv
import gzip
import io
import json
import shutil
import tempfile
import time
import urllib.error
import urllib.request
import zipfile
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from datetime import date, datetime, timedelta, timezone
from pathlib import Path
from typing import Iterator, List, Optional, Tuple, Union

from . import _cache, _dedup


_BASE_URL = "https://img.bitgetimg.com/online/bitget/archive"

_MARKETS = {"spot", "umcbl", "cmcbl"}

_INSTRUMENT_TYPE = {
    "spot":  "spot",
    "umcbl": "perpetual",
    "cmcbl": "perpetual",
}

_SIDE_BUY = 0
_SIDE_SELL = 1

DateLike = Union[str, date]


@dataclass
class ConvertStats:
    trades_written: int = 0
    rows_read: int = 0
    rows_skipped: int = 0
    files_processed: int = 0
    first_ts_ns: int = 0
    last_ts_ns: int = 0
    last_trade_id: int = 0


def _archive_url(symbol: str, market: str, day: date) -> str:
    if market not in _MARKETS:
        raise ValueError(
            f"unknown market '{market}'. expected one of {sorted(_MARKETS)}"
        )
    return f"{_BASE_URL}/{market}/{symbol}/{symbol}-trades-{day.isoformat()}.zip"


def _mirror_path(mirror: Path, symbol: str, market: str, day: date) -> Path:
    return mirror / market / symbol / f"{symbol}-trades-{day.isoformat()}.zip"


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


def _open_csv(path: Path) -> Tuple[io.TextIOBase, "object"]:
    name = path.name.lower()
    if name.endswith(".zip"):
        zf = zipfile.ZipFile(path)
        members = [n for n in zf.namelist() if n.lower().endswith(".csv")]
        if not members:
            zf.close()
            raise ValueError(f"no CSV member inside {path}")
        raw = zf.open(members[0])
        return io.TextIOWrapper(raw, encoding="utf-8", newline=""), zf
    if name.endswith(".gz"):
        f = gzip.open(path, "rt", encoding="utf-8", newline="")
        return f, f
    f = open(path, "r", encoding="utf-8", newline="")
    return f, f


def _iter_rows(path: Path) -> Iterator[Tuple[int, float, float, int, int]]:
    """Yield ``(trade_id, price, qty, ts_ns, side)`` rows from a Bitget
    archive CSV. Skips a leading header row when present."""
    stream, ctx = _open_csv(path)
    try:
        reader = csv.reader(stream)
        for row in reader:
            if not row:
                continue
            try:
                trade_id = int(row[0])
            except (ValueError, IndexError):
                continue
            try:
                price = float(row[1])
                size = float(row[2])
                side_token = row[3].strip().lower() if len(row) > 3 else ""
                ts_ms = int(row[4])
            except (ValueError, IndexError):
                continue
            side = _SIDE_BUY if side_token == "buy" else _SIDE_SELL
            ts_ns = ts_ms * 1_000_000
            yield (trade_id, price, size, ts_ns, side)
    finally:
        try:
            ctx.close()
        except Exception:
            pass


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
        "custom": {},
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
    market: str = "umcbl",
    exchange_id: int = 0,
    exchange_name: str = "bitget",
    append: bool = True,
    max_segment_mb: int = 256,
    compression: str = "none",
    write_metadata: bool = True,
) -> ConvertStats:
    """Convert one Bitget archive CSV (zipped, gzipped, or extracted) into lrvx."""
    import lrvx
    import numpy as np

    csv_path = Path(csv_path).expanduser()
    out_tape = Path(out_tape).expanduser()
    if not csv_path.exists():
        raise FileNotFoundError(csv_path)
    if not symbol_name:
        stem = csv_path.name
        for ext in (".csv.gz", ".csv", ".zip"):
            if stem.endswith(ext):
                stem = stem[: -len(ext)]
                break
        marker = "-trades-"
        idx = stem.find(marker)
        if idx > 0:
            symbol_name = stem[:idx]
        else:
            symbol_name = stem

    rows: List[Tuple[int, float, float, int, int]] = list(_iter_rows(csv_path))
    rows_read = len(rows)
    last_seen_id = max((r[0] for r in rows), default=0)
    rows_skipped = 0
    if append:
        rows, rows_skipped = _dedup.drop_trades_on_tape(rows, out_tape, symbol_id)

    if not rows:
        if write_metadata and rows_read:
            _write_metadata(
                out_tape, exchange_name=exchange_name, market=market,
                symbol_id=symbol_id, symbol_name=symbol_name,
                first_ts_ns=0, last_ts_ns=0, trades_added=0,
            )
        return ConvertStats(rows_read=rows_read,
                            rows_skipped=rows_skipped,
                            files_processed=1,
                            last_trade_id=last_seen_id)

    rows.sort(key=lambda t: t[3])

    n = len(rows)
    ids = np.fromiter((r[0] for r in rows), dtype=np.uint64, count=n)
    prices = np.fromiter((r[1] for r in rows), dtype=np.float64, count=n)
    qty = np.fromiter((r[2] for r in rows), dtype=np.float64, count=n)
    ts_ns = np.fromiter((r[3] for r in rows), dtype=np.int64, count=n)
    sides = np.fromiter((r[4] for r in rows), dtype=np.uint8, count=n)
    sym_ids = np.full(n, int(symbol_id), dtype=np.uint32)

    out_tape.mkdir(parents=True, exist_ok=True)
    writer = lrvx.DataWriter(
        str(out_tape),
        max_segment_mb=int(max_segment_mb),
        exchange_id=int(exchange_id),
        compression=compression,
    )
    try:
        n_written = writer.write_trades(
            exchange_ts_ns=ts_ns, recv_ts_ns=ts_ns,
            prices=prices, quantities=qty,
            trade_ids=ids, symbol_ids=sym_ids, sides=sides,
        )
    finally:
        writer.close()

    first_ts = int(ts_ns[0])
    last_ts = int(ts_ns[-1])

    if write_metadata:
        _write_metadata(
            out_tape, exchange_name=exchange_name, market=market,
            symbol_id=symbol_id, symbol_name=symbol_name,
            first_ts_ns=first_ts, last_ts_ns=last_ts,
            trades_added=int(n_written),
        )

    return ConvertStats(
        trades_written=int(n_written),
        rows_read=rows_read,
        rows_skipped=rows_skipped,
        files_processed=1,
        first_ts_ns=first_ts,
        last_ts_ns=last_ts,
        last_trade_id=int(ids[-1]),
    )


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
    exchange_name: str = "bitget",
    append: bool = True,
    max_segment_mb: int = 256,
    compression: str = "none",
    skip_missing: bool = False,
) -> ConvertStats:
    """Convert a multi-day Bitget range into lrvx. Uses the shared
    archive cache (``~/.lrvx/archive-cache/bitget``) when ``mirror`` is
    omitted."""
    days = list(_iter_days(date_from, date_to))
    if not days:
        return ConvertStats()

    if mirror is None:
        mirror = _cache.cache_root() / "bitget"
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


__all__ = [
    "ConvertStats",
    "trades_to_lrvx",
    "range_to_lrvx",
]
