"""Deribit public archive → lrvx converter.

Deribit exposes per-instrument per-day trade CSVs at
``history.deribit.com``. Each row covers one execution:

    trade_id, timestamp_ms, instrument, side, price, amount,
    mark_price, iv, index_price

`trade_id` is integer and exchange-assigned, used as the lrvx
``trade_id`` for append-safe dedup. `side` is the active flow as
``buy`` / ``sell`` lowercase strings.

Markets covered by this importer:

  * ``perpetual``  — BTC-PERPETUAL, ETH-PERPETUAL, SOL-PERPETUAL, ...
  * ``future``     — dated futures (BTC-29MAR24, ETH-28JUN24, ...)
  * ``option``     — option-chain instruments (BTC-29MAR24-50000-C, ...)

Each call converts ONE instrument per tape. Multi-instrument option-
chain aggregation (glob across all expirys for a given underlying)
is left as a follow-up — the simpler single-instrument convert covers
backtest research that pins to a specific option contract or rolls
through a known series sequentially.

For option instruments, the `mark_price`, `iv`, and `index_price`
columns are preserved as OptionQuote frames alongside the trades and
read back via ``DataReader.read_option_quotes_from``. `iv` is stored as
the archive provides it (Deribit reports trade IV in percent), so the
value round-trips the source and the consumer applies its own unit
convention. Open interest is not in the trade archive and stays 0.
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
from typing import Iterator, List, Optional, Sequence, Tuple, Union

from . import _cache, _dedup


_BASE_URL = "https://history.deribit.com"

_MARKETS = {"perpetual", "future", "option"}

_INSTRUMENT_TYPE = {
    "perpetual": "perpetual",
    "future":    "futures",
    "option":    "option",
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
    return f"{_BASE_URL}/{market}/{symbol}/{symbol}-{day.isoformat()}.csv.gz"


def _mirror_path(mirror: Path, symbol: str, market: str, day: date) -> Path:
    return mirror / market / symbol / f"{symbol}-{day.isoformat()}.csv.gz"


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


def _iter_rows(
    path: Path,
) -> Iterator[Tuple[int, float, float, int, int, float, float, float]]:
    """Yield ``(trade_id, price, qty, ts_ns, side, mark, iv, index)`` rows from
    a Deribit archive CSV. Skips the leading header row.

    Deribit columns (post-2022): trade_id, timestamp_ms, instrument, side,
    price, amount, mark_price, iv, index_price. ``mark``/``index`` are prices;
    ``iv`` is the implied volatility in percent (e.g. 65.5). Missing side
    channels (older trade-only rows) yield NaN."""
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
                ts_ms = int(row[1])
                side_token = row[3].strip().lower() if len(row) > 3 else ""
                price = float(row[4])
                amount = float(row[5])
            except (ValueError, IndexError):
                continue

            def _opt(idx: int) -> float:
                try:
                    return float(row[idx]) if len(row) > idx and row[idx] != "" else float("nan")
                except (ValueError, IndexError):
                    return float("nan")

            mark = _opt(6)
            iv = _opt(7)
            index = _opt(8)
            side = _SIDE_BUY if side_token == "buy" else _SIDE_SELL
            ts_ns = ts_ms * 1_000_000
            yield (trade_id, price, amount, ts_ns, side, mark, iv, index)
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


def _base_quote(name: str, market: str) -> Tuple[str, str]:
    """Pull (base, quote) out of a Deribit instrument id.

    Deribit instruments encode the underlying as the first hyphen-
    separated token: ``BTC-PERPETUAL`` → BTC, ``BTC-29MAR24`` → BTC,
    ``BTC-29MAR24-50000-C`` → BTC. Quote is conventionally USD for
    perp / future / option on Deribit (settlement currency varies,
    but the quote currency for price-quoted instruments is USD)."""
    parts = name.split("-")
    base = parts[0] if parts else ""
    return base, "USD"


def _write_metadata(out_tape: Path, *,
                    exchange_name: str,
                    market: str,
                    symbol_id: int,
                    symbol_name: str,
                    first_ts_ns: int,
                    last_ts_ns: int,
                    trades_added: int) -> None:
    base, quote = _base_quote(symbol_name, market)
    sym_entry = {
        "symbol_id": int(symbol_id),
        "name": symbol_name,
        "base_asset": base,
        "quote_asset": quote,
        "price_precision": 8,
        "qty_precision": 8,
    }
    meta = {
        "recording_id": f"deribit-{market}-{symbol_name}",
        "description": f"Deribit {market} trades archive for {symbol_name}",
        "exchange": exchange_name,
        "exchange_type": "cex",
        "instrument_type": _INSTRUMENT_TYPE.get(market, ""),
        "connector_version": "deribit-archive-import",
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
    market: str = "perpetual",
    exchange_id: int = 0,
    exchange_name: str = "deribit",
    append: bool = True,
    max_segment_mb: int = 256,
    compression: str = "none",
    write_metadata: bool = True,
) -> ConvertStats:
    """Convert one Deribit archive CSV (zipped, gzipped, or extracted) into lrvx."""
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
        # Deribit file convention: `<INSTRUMENT>-<YYYY-MM-DD>`.
        if len(stem) > 10 and stem[-10] == "-":
            stem = stem[:-11]   # strip "-YYYY-MM-DD"
        symbol_name = stem

    def _option_quotes(writer, batches) -> None:
        # For option instruments, preserve the mark/iv/index side channels as
        # OptionQuote frames, after every trade of the file. iv is stored
        # exactly as the Deribit archive provides it (Deribit reports trade IV
        # in percent), so the tape value round-trips the source; a consumer
        # applies its own unit convention. Open interest is not in the trade
        # archive, so it stays 0. Rows missing every side channel (all NaN)
        # are skipped.
        for batch in batches:
            mark, iv, index = batch["mark"], batch["iv"], batch["index"]
            valid = ~(np.isnan(mark) & np.isnan(iv) & np.isnan(index))
            if not valid.any():
                continue
            ts = batch["ts"][valid].astype(np.int64)
            writer.write_option_quotes(
                exchange_ts_ns=ts, recv_ts_ns=ts,
                mark_prices=np.nan_to_num(mark[valid]),
                index_prices=np.nan_to_num(index[valid]),
                ivs=np.nan_to_num(iv[valid]),
                open_interest=np.zeros(int(valid.sum()), dtype=np.float64),
                symbol_ids=np.full(int(valid.sum()), int(symbol_id),
                                   dtype=np.uint32),
            )

    with _dedup.TradeFile(_iter_rows(csv_path),
                          _dedup.trade_dtype(extra=("mark", "iv", "index")),
                          sort=True) as rows:
        done = _dedup.write_trade_file(
            rows, out_tape, append=append, symbol_id=symbol_id,
            exchange_id=exchange_id, max_segment_mb=max_segment_mb,
            compression=compression,
            then=_option_quotes if market == "option" else None)
        rows_read, rows_skipped = rows.rows_read, rows.rows_skipped
        kept, last_seen_id = rows.rows_kept, rows.id_max

    if not kept:
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

    if write_metadata:
        _write_metadata(
            out_tape, exchange_name=exchange_name, market=market,
            symbol_id=symbol_id, symbol_name=symbol_name,
            first_ts_ns=done.first_ts_ns, last_ts_ns=done.last_ts_ns,
            trades_added=done.count,
        )

    return ConvertStats(
        trades_written=done.count,
        rows_read=rows_read,
        rows_skipped=rows_skipped,
        files_processed=1,
        first_ts_ns=done.first_ts_ns,
        last_ts_ns=done.last_ts_ns,
        last_trade_id=done.last_id,
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
    exchange_name: str = "deribit",
    append: bool = True,
    max_segment_mb: int = 256,
    compression: str = "none",
    skip_missing: bool = False,
) -> ConvertStats:
    """Convert a multi-day Deribit range into lrvx. Uses the shared
    archive cache (``~/.lrvx/archive-cache/deribit``) when ``mirror``
    is omitted."""
    days = list(_iter_days(date_from, date_to))
    if not days:
        return ConvertStats()

    if mirror is None:
        mirror = _cache.cache_root() / "deribit"
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


# ─── Option chain loading ────────────────────────────────────────────────────

def parse_option_instrument(name: str) -> Tuple[str, date, float, str]:
    """Parse a Deribit option instrument id into (underlying, expiry, strike,
    option_type). ``BTC-29MAR24-50000-C`` -> ("BTC", date(2024,3,29), 50000.0,
    "C"). option_type is "C" (call) or "P" (put). Raises ValueError on a
    non-option id (perp/future have a different shape)."""
    parts = name.split("-")
    if len(parts) != 4:
        raise ValueError(f"not an option instrument: {name!r}")
    underlying, expiry_s, strike_s, opt = parts
    opt = opt.upper()
    if opt not in ("C", "P"):
        raise ValueError(f"unknown option type {opt!r} in {name!r}")
    # Deribit expiry is DDMMMYY, e.g. 29MAR24. strptime wants title-case month.
    expiry = datetime.strptime(expiry_s.title(), "%d%b%y").date()
    return underlying, expiry, float(strike_s), opt


@dataclass(frozen=True)
class OptionContract:
    instrument: str
    underlying: str
    expiry: date
    strike: float
    option_type: str  # "C" or "P"
    tape: Path


class OptionChain:
    """A queryable set of option contracts, each backed by its own lrvx tape.
    Filter by expiry / strike / type, take an as-of-date slice (contracts not
    yet expired), and walk expiries for series rolls."""

    def __init__(self, contracts: Sequence[OptionContract]) -> None:
        self._contracts: List[OptionContract] = list(contracts)

    def __len__(self) -> int:
        return len(self._contracts)

    def __iter__(self) -> Iterator[OptionContract]:
        return iter(self._contracts)

    def expiries(self) -> List[date]:
        return sorted({c.expiry for c in self._contracts})

    def strikes(self, expiry: Optional[date] = None) -> List[float]:
        pool = self._contracts if expiry is None else [c for c in self._contracts
                                                       if c.expiry == expiry]
        return sorted({c.strike for c in pool})

    def query(self, expiry: Optional[date] = None, strike: Optional[float] = None,
              option_type: Optional[str] = None) -> List[OptionContract]:
        opt = option_type.upper() if option_type else None
        return [c for c in self._contracts
                if (expiry is None or c.expiry == expiry)
                and (strike is None or c.strike == strike)
                and (opt is None or c.option_type == opt)]

    def get(self, expiry: date, strike: float, option_type: str) -> Optional[OptionContract]:
        hits = self.query(expiry=expiry, strike=strike, option_type=option_type)
        return hits[0] if hits else None

    def as_of(self, on: DateLike) -> "OptionChain":
        """Contracts live on ``on`` (expiry on or after that date)."""
        d = _coerce_day(on)
        return OptionChain([c for c in self._contracts if c.expiry >= d])

    def next_expiry_after(self, on: DateLike) -> Optional[date]:
        """First expiry strictly after ``on`` — the contract a series rolls into."""
        d = _coerce_day(on)
        later = [e for e in self.expiries() if e > d]
        return later[0] if later else None


def load_option_chain(
    instruments: Sequence[str],
    day: DateLike,
    out_root: Union[str, Path],
    *,
    mirror: Union[str, Path],
    exchange_id: int = 0,
    compression: str = "none",
) -> OptionChain:
    """Convert each option instrument's archive for ``day`` into its own tape
    under ``out_root/<instrument>`` and return an OptionChain over them. CSVs are
    read from ``mirror`` (Deribit layout) and downloaded there if absent. Each
    instrument gets a distinct symbol_id (its index + 1)."""
    day = _coerce_day(day)
    out_root = Path(out_root).expanduser()
    mirror = Path(mirror).expanduser()
    contracts: List[OptionContract] = []
    for i, inst in enumerate(instruments):
        underlying, expiry, strike, opt = parse_option_instrument(inst)
        csv_path = _mirror_path(mirror, inst, "option", day)
        if not csv_path.exists():
            _download(_archive_url(inst, "option", day), csv_path)
        tape = out_root / inst
        trades_to_lrvx(
            csv_path, tape, symbol_id=i + 1, symbol_name=inst, market="option",
            exchange_id=exchange_id, compression=compression,
        )
        contracts.append(OptionContract(inst, underlying, expiry, strike, opt, tape))
    return OptionChain(contracts)


__all__ = [
    "ConvertStats",
    "trades_to_lrvx",
    "range_to_lrvx",
    "parse_option_instrument",
    "OptionContract",
    "OptionChain",
    "load_option_chain",
]
