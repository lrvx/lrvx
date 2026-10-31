"""Binance public archive → lrvx converter.

``data.binance.vision`` publishes daily aggTrades zip archives going
back 2+ years for spot, USDT-margined futures, and coin-margined
futures. The on-disk CSV layout is well-known and stable:

    agg_trade_id, price, quantity, first_trade_id, last_trade_id,
    transact_time_ms, is_buyer_maker, is_best_match

Some daily files prepend a header row; the reader skips it
transparently. Side encoding follows the existing lrvx convention:

  * ``is_buyer_maker = true``  → buyer rests as maker → active flow is
    selling → ``Side::SELL`` (1)
  * ``is_buyer_maker = false`` → seller rests as maker → active flow is
    buying  → ``Side::BUY``  (0)

The converter writes through ``lrvx.DataWriter`` and is append-safe:
re-running the same day is a no-op because dedup keys on
``agg_trade_id`` (stable, exchange-assigned).

A standalone ``metadata.json`` is written alongside the segments so the
result keys cleanly into ``MergedTapeReader`` against other lrvx
tapes — exchange = "binance", instrument_type derived from ``market``.
"""
from __future__ import annotations

import csv
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
from typing import Iterator, List, Optional, Sequence, Tuple, Union

# Market identifier → URL path prefix on data.binance.vision for the
# trade stream (aggTrades) and the two book streams (bookTicker /
# bookDepth). Layout in the public archive:
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

# Per-book archive product names on data.binance.vision. `bookTicker`
# is best bid/ask only; `bookDepth` is top-20 levels per side.
_BOOK_PRODUCT_NAMES = {
    "t1":       "bookTicker",
    "depth20":  "bookDepth",
}

# Markets that publish each book archive. `bookDepth` is only on
# um-futures today; `bookTicker` is published for every segment.
_BOOK_MARKETS = {
    "t1":       {"spot", "um-futures", "cm-futures"},
    "depth20":  {"um-futures"},
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


@dataclass
class ConvertStats:
    """Per-invocation summary returned by the converters."""

    trades_written: int = 0
    rows_read: int = 0
    rows_skipped: int = 0
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


def _download(url: str, dest: Path, *, retries: int = 3, backoff: float = 2.0) -> None:
    """Download ``url`` to ``dest`` atomically with retry.

    Uses a ``.tmp`` sibling and ``rename`` so a partial fetch does not
    poison a long-running mirror.
    """
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


# ── CSV parsing ─────────────────────────────────────────────────────


def _open_csv_member(path: Path) -> Tuple[io.TextIOBase, "object"]:
    """Open the single CSV member inside a Binance aggTrades zip (or a
    bare CSV file). Returns the text stream + a context object the
    caller must close."""
    if path.suffix.lower() == ".zip":
        zf = zipfile.ZipFile(path)
        names = [n for n in zf.namelist() if n.lower().endswith(".csv")]
        if not names:
            zf.close()
            raise ValueError(f"no CSV member inside {path}")
        raw = zf.open(names[0])
        return io.TextIOWrapper(raw, encoding="ascii", newline=""), zf
    fh = open(path, "r", encoding="ascii", newline="")
    return fh, fh


def _iter_rows(path: Path) -> Iterator[Tuple[int, float, float, int, bool]]:
    """Yield ``(agg_id, price, qty, ts_ms, is_buyer_maker)`` rows from a
    Binance aggTrades CSV (zipped or extracted). The first row is
    skipped transparently if it is a header."""
    stream, ctx = _open_csv_member(path)
    try:
        reader = csv.reader(stream)
        for row in reader:
            if not row:
                continue
            try:
                agg_id = int(row[0])
            except ValueError:
                # Header line such as
                # "agg_trade_id,price,quantity,first_trade_id,..."
                continue
            price = float(row[1])
            qty = float(row[2])
            ts_ms = int(row[5])
            maker = row[6].strip().lower() in ("true", "1")
            yield (agg_id, price, qty, ts_ms, maker)
    finally:
        try:
            ctx.close()
        except Exception:
            pass


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
        "custom": {},
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
            meta = existing
        except (OSError, ValueError, KeyError):
            pass

    out_tape.mkdir(parents=True, exist_ok=True)
    meta_path.write_text(json.dumps(meta, indent=2, sort_keys=True))


# ── Single-day conversion ───────────────────────────────────────────


def _existing_max_trade_id(out_tape: Path, symbol_id: int) -> int:
    """Return the highest existing trade_id for ``symbol_id`` in the
    tape, or 0 if the tape is empty / unreadable. Used for dedup so a
    re-run of the same day is a no-op."""
    if not out_tape.exists():
        return 0
    try:
        import lrvx
        import numpy as np

        reader = lrvx.DataReader(str(out_tape))
        trades = reader.read_trades()
        if trades.size == 0:
            return 0
        ids = np.asarray(trades["trade_id"])
        syms = np.asarray(trades["symbol_id"])
        mask = syms == symbol_id
        return int(ids[mask].max()) if mask.any() else 0
    except Exception:
        return 0


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
) -> ConvertStats:
    """Convert one Binance aggTrades day (zipped or extracted CSV) into
    a lrvx tape.

    ``symbol_name`` defaults to the file's basename leading token if
    omitted. Dedup keys on ``agg_trade_id`` so re-running a day already
    present in ``out_tape`` returns ``trades_written=0`` and updates
    only ``rows_skipped``.
    """
    import lrvx
    import numpy as np

    csv_path = Path(csv_path).expanduser()
    out_tape = Path(out_tape).expanduser()
    if not csv_path.exists():
        raise FileNotFoundError(csv_path)
    if not symbol_name:
        # "BTCUSDT-aggTrades-2024-01-15.zip" → "BTCUSDT"
        symbol_name = csv_path.stem.split("-", 1)[0]

    last_id = _existing_max_trade_id(out_tape, symbol_id) if append else 0

    # Stream-then-batch: the daily file is small enough (typically
    # <100 MB extracted) that one numpy round-trip beats the per-row
    # call overhead of write_trade.
    rows: List[Tuple[int, float, float, int, bool]] = []
    rows_read = 0
    rows_skipped = 0
    for r in _iter_rows(csv_path):
        rows_read += 1
        # last_id == 0 doubles as "nothing archived yet", so a real
        # aggTradeId of 0 was skipped as already-seen. The sibling
        # archives (bybit, deribit, bitget) already carry this guard.
        if r[0] <= last_id and r[0] != 0:
            rows_skipped += 1
            continue
        rows.append(r)

    if not rows:
        if write_metadata and rows_read:
            _write_metadata(
                out_tape,
                exchange_name=exchange_name,
                market=market,
                symbol_id=symbol_id,
                symbol_name=symbol_name,
                first_ts_ns=0,
                last_ts_ns=0,
                trades_added=0,
            )
        return ConvertStats(
            trades_written=0,
            rows_read=rows_read,
            rows_skipped=rows_skipped,
            files_processed=1,
            last_trade_id=last_id,
        )

    n = len(rows)
    agg_ids = np.fromiter((r[0] for r in rows), dtype=np.int64, count=n)
    prices = np.fromiter((r[1] for r in rows), dtype=np.float64, count=n)
    qty = np.fromiter((r[2] for r in rows), dtype=np.float64, count=n)
    ts_ms = np.fromiter((r[3] for r in rows), dtype=np.int64, count=n)
    maker = np.fromiter((1 if r[4] else 0 for r in rows), dtype=np.uint8, count=n)
    exchange_ts_ns = ts_ms * 1_000_000
    sides = np.where(maker.astype(bool), _SIDE_SELL, _SIDE_BUY).astype(np.uint8)
    trade_ids = agg_ids.astype(np.uint64)
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
            exchange_ts_ns=exchange_ts_ns,
            recv_ts_ns=exchange_ts_ns,
            prices=prices,
            quantities=qty,
            trade_ids=trade_ids,
            symbol_ids=sym_ids,
            sides=sides,
        )
    finally:
        writer.close()

    first_ts = int(exchange_ts_ns[0])
    last_ts = int(exchange_ts_ns[-1])

    if write_metadata:
        _write_metadata(
            out_tape,
            exchange_name=exchange_name,
            market=market,
            symbol_id=symbol_id,
            symbol_name=symbol_name,
            first_ts_ns=first_ts,
            last_ts_ns=last_ts,
            trades_added=int(n_written),
        )

    return ConvertStats(
        trades_written=int(n_written),
        rows_read=rows_read,
        rows_skipped=rows_skipped,
        files_processed=1,
        first_ts_ns=first_ts,
        last_ts_ns=last_ts,
        last_trade_id=int(trade_ids[-1]),
    )


# ── Multi-day conversion ────────────────────────────────────────────


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
    exchange_name: str = "binance",
    append: bool = True,
    max_segment_mb: int = 256,
    compression: str = "none",
    skip_missing: bool = False,
) -> ConvertStats:
    """Convert a date range from a local mirror or HTTP into lrvx.

    With ``mirror`` set, missing zips are downloaded into the mirror
    layout (``<mirror>/<market>/<symbol>/<symbol>-aggTrades-<YYYY-MM-DD>.zip``)
    so a follow-up call reuses the cached files. Without ``mirror``, a
    tempdir is used and discarded.

    Downloads run up to ``parallel`` connections in flight; the
    lrvx write step is serial so the writer stays append-safe.
    """
    days = list(_iter_days(date_from, date_to))
    if not days:
        return ConvertStats()

    cleanup = False
    if mirror is None:
        mirror = Path(tempfile.mkdtemp(prefix="binance_archive_"))
        cleanup = True
    mirror = Path(mirror).expanduser()

    def _ensure(day: date) -> Tuple[date, Optional[Path]]:
        local = _mirror_path(mirror, symbol, market, day)
        if local.exists():
            return day, local
        try:
            _download(_aggtrades_url(symbol, market, day), local)
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
        s = aggtrades_to_lrvx(
            path,
            out_tape,
            symbol_id=symbol_id,
            symbol_name=symbol,
            market=market,
            exchange_id=exchange_id,
            exchange_name=exchange_name,
            append=append,
            max_segment_mb=max_segment_mb,
            compression=compression,
            # Skip per-day metadata; one final write at the end captures
            # the full range range and total count.
            write_metadata=False,
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
        exchange_name=exchange_name,
        market=market,
        symbol_id=symbol_id,
        symbol_name=symbol,
        first_ts_ns=agg.first_ts_ns,
        last_ts_ns=agg.last_ts_ns,
        trades_added=agg.trades_written,
    )

    if cleanup:
        shutil.rmtree(mirror, ignore_errors=True)

    return agg


__all__ = [
    "ConvertStats",
    "BookConvertStats",
    "aggtrades_to_lrvx",
    "range_to_lrvx",
    "t1_to_lrvx",
    "depth20_to_lrvx",
    "range_book_to_lrvx",
]


# ════════════════════════════════════════════════════════════════════
# Book archives (T1 / bookTicker + depth20 / bookDepth)
# ════════════════════════════════════════════════════════════════════
#
# Binance publishes two complementary book products on
# data.binance.vision:
#
#   * bookTicker (alias: "t1") — best bid / ask snapshot per update,
#     one row per tick. Available for spot / um-futures / cm-futures.
#   * bookDepth (alias: "depth20") — top-20 levels per side per tick.
#     Today only published for um-futures. Heavier (~10x aggTrades)
#     and requires snapshot-to-delta encoding to compress on disk.
#
# The two converters share the parse / write spine of the aggTrades
# importer above (zip member open, append-safe dedup, metadata merge).
# Book events are delta-encoded against the previous snapshot so the
# resulting lrvx plays back through MergedTapeReader and the
# existing book aggregator surface without modification.


@dataclass
class BookConvertStats:
    """Per-invocation summary for the book converters."""

    snapshots_written: int = 0
    deltas_written: int = 0
    rows_read: int = 0
    rows_skipped: int = 0
    files_processed: int = 0
    first_ts_ns: int = 0
    last_ts_ns: int = 0
    last_update_id: int = 0


def _aggregate_book_stats(agg: BookConvertStats, add: BookConvertStats) -> None:
    agg.snapshots_written += add.snapshots_written
    agg.deltas_written += add.deltas_written
    agg.rows_read += add.rows_read
    agg.rows_skipped += add.rows_skipped
    agg.files_processed += add.files_processed
    if add.first_ts_ns and (agg.first_ts_ns == 0 or add.first_ts_ns < agg.first_ts_ns):
        agg.first_ts_ns = add.first_ts_ns
    if add.last_ts_ns > agg.last_ts_ns:
        agg.last_ts_ns = add.last_ts_ns
    if add.last_update_id > agg.last_update_id:
        agg.last_update_id = add.last_update_id


def _book_archive_url(symbol: str, market: str, book_type: str, day: date) -> str:
    if market not in _MARKET_SEGMENTS:
        raise ValueError(
            f"unknown market '{market}'. expected one of {sorted(_MARKET_SEGMENTS)}"
        )
    if book_type not in _BOOK_PRODUCT_NAMES:
        raise ValueError(
            f"unknown book_type '{book_type}'. expected 't1' or 'depth20'"
        )
    if market not in _BOOK_MARKETS[book_type]:
        raise ValueError(
            f"book_type '{book_type}' is not published for market '{market}'"
        )
    seg = _MARKET_SEGMENTS[market]
    product = _BOOK_PRODUCT_NAMES[book_type]
    return f"{_BASE_URL}/{seg}/{product}/{symbol}/{symbol}-{product}-{day.isoformat()}.zip"


def _book_mirror_path(mirror: Path, symbol: str, market: str,
                      book_type: str, day: date) -> Path:
    product = _BOOK_PRODUCT_NAMES[book_type]
    return mirror / market / product / symbol / f"{symbol}-{product}-{day.isoformat()}.zip"


def _existing_book_state(out_tape: Path, symbol_id: int) -> Tuple[int, int, dict, dict]:
    """Inspect the existing tape and reconstruct (last_seq, last_ts_ns,
    bid_levels, ask_levels) so an append can dedup by seq and re-base the
    delta encoder on the right starting state. Returns
    (0, 0, {}, {}) if the tape is empty / unreadable."""
    if not out_tape.exists():
        return 0, 0, {}, {}
    try:
        import lrvx
        import numpy as np

        reader = lrvx.DataReader(str(out_tape))
        headers, levels = reader.read_book_updates()
        if headers.size == 0:
            return 0, 0, {}, {}
        # Walk events for this symbol in order, applying snapshot /
        # delta semantics so we know the latest ladder for the dedup
        # baseline.
        bids: dict[int, int] = {}
        asks: dict[int, int] = {}
        last_seq = 0
        last_ts_ns = 0
        for h in headers:
            if int(h["symbol_id"]) != symbol_id:
                continue
            # DataReader rows carry the record byte (0 snapshot, 1 delta),
            # not the frame type; == 2 was never true here.
            is_snapshot = int(h["event_type"]) == 0
            n_bid = int(h["bid_count"])
            n_ask = int(h["ask_count"])
            off = int(h["level_offset"])
            if is_snapshot:
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
            seq = int(h["seq"])
            ts = int(h["exchange_ts_ns"])
            if seq > last_seq:
                last_seq = seq
            if ts > last_ts_ns:
                last_ts_ns = ts
        return last_seq, last_ts_ns, bids, asks
    except Exception:
        return 0, 0, {}, {}


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


def _open_csv_lines(path: Path, *, numeric_col_idx: int = 0) -> Iterator[List[str]]:
    """Iterate CSV rows from a zipped or extracted file, transparently
    skipping a leading header row when one is present.

    The header check parses ``row[numeric_col_idx]`` as a float; any
    row where that conversion fails is dropped. Numeric column varies
    by archive product (``aggTrades`` → col 0 is agg_trade_id,
    ``bookTicker`` → col 0 is update_id, ``bookDepth`` long-format →
    col 0 is the symbol string, so callers index into col 1)."""
    stream, ctx = _open_csv_member(path)
    try:
        reader = csv.reader(stream)
        for row in reader:
            if not row:
                continue
            try:
                float(row[numeric_col_idx])
            except (ValueError, IndexError):
                # Header line or malformed row.
                continue
            yield row
    finally:
        try:
            ctx.close()
        except Exception:
            pass


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
    """
    import lrvx

    csv_path = Path(csv_path).expanduser()
    out_tape = Path(out_tape).expanduser()
    if not csv_path.exists():
        raise FileNotFoundError(csv_path)
    if not symbol_name:
        symbol_name = csv_path.stem.split("-", 1)[0]

    last_seq, last_ts_ns, prev_bids, prev_asks = (
        _existing_book_state(out_tape, symbol_id) if append else (0, 0, {}, {})
    )

    rows_read = 0
    rows_skipped = 0
    snapshots_written = 0
    deltas_written = 0
    first_emitted_ts = 0
    last_emitted_ts = last_ts_ns
    last_emitted_seq = last_seq

    out_tape.mkdir(parents=True, exist_ok=True)
    w = lrvx.DataWriter(
        str(out_tape),
        max_segment_mb=int(max_segment_mb),
        exchange_id=int(exchange_id),
        compression=compression,
    )

    is_first_emit = (not prev_bids) and (not prev_asks)
    try:
        for row in _open_csv_lines(csv_path):
            rows_read += 1
            update_id = int(row[0])
            if update_id <= last_seq:
                rows_skipped += 1
                continue
            try:
                bid_p = float(row[1])
                bid_q = float(row[2])
                ask_p = float(row[3])
                ask_q = float(row[4])
                ts_ms = int(row[5])
            except (ValueError, IndexError):
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
                # Initial snapshot: emit complete current top-of-book.
                w.write_book(
                    exchange_ts_ns=ts_ns, recv_ts_ns=ts_ns,
                    seq=int(update_id), symbol_id=int(symbol_id),
                    is_snapshot=True,
                    bids=_levels_array(list(cur_bids.items())),
                    asks=_levels_array(list(cur_asks.items())),
                )
                is_first_emit = False
                snapshots_written += 1
                first_emitted_ts = first_emitted_ts or ts_ns
            else:
                bid_delta, ask_delta = _diff_book(
                    prev_bids, cur_bids, prev_asks, cur_asks)
                if not bid_delta and not ask_delta:
                    rows_skipped += 1
                    continue
                w.write_book(
                    exchange_ts_ns=ts_ns, recv_ts_ns=ts_ns,
                    seq=int(update_id), symbol_id=int(symbol_id),
                    is_snapshot=False,
                    bids=_levels_array(bid_delta),
                    asks=_levels_array(ask_delta),
                )
                deltas_written += 1
                first_emitted_ts = first_emitted_ts or ts_ns

            prev_bids = cur_bids
            prev_asks = cur_asks
            last_emitted_ts = ts_ns
            last_emitted_seq = int(update_id)
    finally:
        w.close()

    stats = BookConvertStats(
        snapshots_written=snapshots_written,
        deltas_written=deltas_written,
        rows_read=rows_read,
        rows_skipped=rows_skipped,
        files_processed=1,
        first_ts_ns=first_emitted_ts,
        last_ts_ns=last_emitted_ts,
        last_update_id=last_emitted_seq,
    )

    total_writes = snapshots_written + deltas_written
    if write_metadata and total_writes > 0:
        _write_book_metadata(
            out_tape,
            exchange_name=exchange_name,
            market=market,
            symbol_id=symbol_id,
            symbol_name=symbol_name,
            first_ts_ns=first_emitted_ts,
            last_ts_ns=last_emitted_ts,
            updates_added=total_writes,
            depth=1,
        )

    return stats


# ── depth20 / bookDepth ────────────────────────────────────────────


def depth20_to_lrvx(
    csv_path: Union[str, Path],
    out_tape: Union[str, Path],
    *,
    levels: int = 20,
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
    """Convert one Binance bookDepth day (zipped or extracted CSV) into
    delta-encoded book events.

    Binance bookDepth CSV is in long format: one row per (event,
    side, price) tuple. Columns:

        symbol, timestamp_ms, first_update_id, last_update_id, side,
        update_type, price, qty

    where ``side`` is "b" / "a" (or "BID" / "ASK") and ``update_type``
    is "snap" for the initial frame and "set" / "del" for subsequent
    updates. The converter groups rows by ``last_update_id`` and emits
    one book event per group: full snapshot for the first
    ``update_type == 'snap'`` group, delta for every group after.
    Unchanged ladders are skipped.
    """
    import lrvx

    csv_path = Path(csv_path).expanduser()
    out_tape = Path(out_tape).expanduser()
    if not csv_path.exists():
        raise FileNotFoundError(csv_path)
    if not symbol_name:
        symbol_name = csv_path.stem.split("-", 1)[0]

    last_seq, last_ts_ns, prev_bids, prev_asks = (
        _existing_book_state(out_tape, symbol_id) if append else (0, 0, {}, {})
    )

    out_tape.mkdir(parents=True, exist_ok=True)
    w = lrvx.DataWriter(
        str(out_tape),
        max_segment_mb=int(max_segment_mb),
        exchange_id=int(exchange_id),
        compression=compression,
    )

    rows_read = 0
    rows_skipped = 0
    snapshots = 0
    deltas = 0
    first_emitted_ts = 0
    last_emitted_ts = last_ts_ns
    last_emitted_seq = last_seq

    def _flush(group_id: int, group_ts_ms: int, group_is_snap: bool,
               cur_bids: dict, cur_asks: dict) -> None:
        nonlocal prev_bids, prev_asks, snapshots, deltas
        nonlocal first_emitted_ts, last_emitted_ts, last_emitted_seq
        if group_id <= last_seq:
            return  # dedup against existing
        ts_ns = group_ts_ms * 1_000_000
        if group_is_snap or (not prev_bids and not prev_asks):
            w.write_book(
                exchange_ts_ns=ts_ns, recv_ts_ns=ts_ns,
                seq=int(group_id), symbol_id=int(symbol_id),
                is_snapshot=True,
                bids=_levels_array(list(cur_bids.items())),
                asks=_levels_array(list(cur_asks.items())),
            )
            snapshots += 1
        else:
            bid_delta, ask_delta = _diff_book(
                prev_bids, cur_bids, prev_asks, cur_asks)
            if not bid_delta and not ask_delta:
                return
            w.write_book(
                exchange_ts_ns=ts_ns, recv_ts_ns=ts_ns,
                seq=int(group_id), symbol_id=int(symbol_id),
                is_snapshot=False,
                bids=_levels_array(bid_delta),
                asks=_levels_array(ask_delta),
            )
            deltas += 1
        prev_bids = dict(cur_bids)
        prev_asks = dict(cur_asks)
        first_emitted_ts = first_emitted_ts or ts_ns
        last_emitted_ts = ts_ns
        last_emitted_seq = int(group_id)

    cur_group_id: Optional[int] = None
    cur_group_ts_ms: int = 0
    cur_group_is_snap: bool = False
    cur_bids_group: dict = {}
    cur_asks_group: dict = {}

    try:
        for row in _open_csv_lines(csv_path, numeric_col_idx=1):
            rows_read += 1
            # Binance bookDepth long-format CSV: [symbol, ts_ms,
            # first_update_id, last_update_id, side, update_type,
            # price, qty]. Be defensive: accept either lowercase or
            # uppercase side / update_type tokens.
            try:
                ts_ms = int(row[1])
                last_update_id = int(row[3])
                side = row[4].strip().lower()
                update_type = row[5].strip().lower()
                price = float(row[6])
                qty = float(row[7])
            except (ValueError, IndexError):
                rows_skipped += 1
                continue

            is_snap = update_type in ("snap", "snapshot", "s")

            if cur_group_id is None or cur_group_id != last_update_id:
                if cur_group_id is not None:
                    _flush(cur_group_id, cur_group_ts_ms,
                           cur_group_is_snap,
                           cur_bids_group, cur_asks_group)
                cur_group_id = last_update_id
                cur_group_ts_ms = ts_ms
                cur_group_is_snap = is_snap
                cur_bids_group = dict(prev_bids) if not is_snap else {}
                cur_asks_group = dict(prev_asks) if not is_snap else {}

            price_raw = int(round(price * 1e8))
            qty_raw = int(round(qty * 1e8))
            target = cur_bids_group if side in ("b", "bid") else cur_asks_group
            if qty_raw == 0:
                target.pop(price_raw, None)
            else:
                target[price_raw] = qty_raw
            # Bookdepth in some archives caps to N levels; trim to
            # `levels` per side so the ladder does not grow unbounded
            # if the source publishes a wider depth.
            if len(target) > levels:
                if target is cur_bids_group:
                    keep = sorted(target.items(), key=lambda kv: -kv[0])[:levels]
                else:
                    keep = sorted(target.items(), key=lambda kv: kv[0])[:levels]
                target.clear()
                target.update(keep)

        if cur_group_id is not None:
            _flush(cur_group_id, cur_group_ts_ms,
                   cur_group_is_snap,
                   cur_bids_group, cur_asks_group)
    finally:
        w.close()

    stats = BookConvertStats(
        snapshots_written=snapshots,
        deltas_written=deltas,
        rows_read=rows_read,
        rows_skipped=rows_skipped,
        files_processed=1,
        first_ts_ns=first_emitted_ts,
        last_ts_ns=last_emitted_ts,
        last_update_id=last_emitted_seq,
    )

    if write_metadata and (snapshots + deltas) > 0:
        _write_book_metadata(
            out_tape,
            exchange_name=exchange_name,
            market=market,
            symbol_id=symbol_id,
            symbol_name=symbol_name,
            first_ts_ns=first_emitted_ts,
            last_ts_ns=last_emitted_ts,
            updates_added=snapshots + deltas,
            depth=int(levels),
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
    levels: int = 20,
    symbol_id: int = 1,
    exchange_id: int = 0,
    exchange_name: str = "binance",
    append: bool = True,
    max_segment_mb: int = 256,
    compression: str = "none",
    skip_missing: bool = False,
) -> BookConvertStats:
    """Convert a multi-day book range from a local mirror or HTTP into
    lrvx book events. Downloads run up to ``parallel`` connections
    in flight (default 2 — depth files are ~10x aggTrades); conversion
    is serial so the writer stays append-safe."""
    if book_type not in _BOOK_PRODUCT_NAMES:
        raise ValueError(
            f"unknown book_type '{book_type}'. expected 't1' or 'depth20'"
        )

    days = list(_iter_days(date_from, date_to))
    if not days:
        return BookConvertStats()

    cleanup = False
    if mirror is None:
        mirror = Path(tempfile.mkdtemp(prefix="binance_book_archive_"))
        cleanup = True
    mirror = Path(mirror).expanduser()

    def _ensure(day: date) -> Tuple[date, Optional[Path]]:
        local = _book_mirror_path(mirror, symbol, market, book_type, day)
        if local.exists():
            return day, local
        try:
            _download(_book_archive_url(symbol, market, book_type, day), local)
        except Exception:
            if skip_missing:
                return day, None
            raise
        return day, local

    pairs: List[Tuple[date, Optional[Path]]] = []
    with ThreadPoolExecutor(max_workers=max(1, int(parallel))) as ex:
        for pair in ex.map(_ensure, days):
            pairs.append(pair)

    agg = BookConvertStats()
    convert = t1_to_lrvx if book_type == "t1" else depth20_to_lrvx
    for day, path in pairs:
        if path is None:
            continue
        if book_type == "t1":
            stats = t1_to_lrvx(
                path, out_tape,
                symbol_id=symbol_id, symbol_name=symbol, market=market,
                exchange_id=exchange_id, exchange_name=exchange_name,
                append=append, max_segment_mb=max_segment_mb,
                compression=compression, write_metadata=False,
            )
        else:
            stats = depth20_to_lrvx(
                path, out_tape, levels=levels,
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
            depth=int(levels) if book_type == "depth20" else 1,
        )

    if cleanup:
        shutil.rmtree(mirror, ignore_errors=True)

    return agg
