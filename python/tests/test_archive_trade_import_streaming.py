"""Trade archive importers: memory bounded by a batch, same tape as a
straightforward whole-file import.

For each of the five trade importers (binance aggTrades, bybit, okx,
bitget, deribit) a synthetic day is written in the venue's CSV layout
and imported. The tape is decoded segment by segment, frame by frame,
straight from the bytes, and compared with what an independent,
whole-file reading of the same CSV says the tape must hold:

  * the rows whose trade id is not already on the tape for the symbol
    within the file's time range,
  * stably sorted by timestamp (binance keeps file order),
  * one new segment per import that writes anything,
  * for deribit options, every trade frame first, then the option-quote
    frames of the written rows that carry a mark / iv / index.

Files come newest-first, shuffled with tied timestamps, and in order,
so the bounded path that merges sorted runs is exercised alongside the
in-order one. The batch size is patched small so a day of a few
thousand rows spans many batches.

The memory test imports a day and re-imports it under tracemalloc and
bounds the peak far below what holding the day as Python tuples costs.

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_archive_trade_import_streaming.py
"""
from __future__ import annotations

import gzip
import hashlib
import io
import random
import struct
import sys
import tempfile
import tracemalloc
import unittest
import uuid
import zipfile
from datetime import date, datetime, timedelta, timezone
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[2]
for cand in ("build/python", "build-py312/python"):
    p = REPO_ROOT / cand
    if p.is_dir():
        sys.path.insert(0, str(p))
        break

import lrvx  # noqa: E402,F401

from lrvx.archives import _dedup, binance, bitget, bybit, deribit, okx  # noqa: E402

_DAY0 = date(2024, 1, 15)
_SYMBOL_ID = 7
_SCALE = 100_000_000


# ── Synthetic days ──────────────────────────────────────────────────


def _day_ms(day: date) -> int:
    dt = datetime(day.year, day.month, day.day, tzinfo=timezone.utc)
    return int(dt.timestamp()) * 1000


def _make_day(seed: int, day_index: int, n: int, order: str) -> list:
    """Rows ``(trade_id, ts_ms, price, qty, side, mark, iv, index)`` for
    one day, in the order the file will list them."""
    rng = random.Random(seed * 1_000 + day_index)
    base = _day_ms(_DAY0 + timedelta(days=day_index))
    stamps = []
    t = base + rng.randrange(1000)
    for _ in range(n):
        if stamps and rng.random() < 0.2:
            stamps.append(stamps[-1])           # tie with the previous trade
        else:
            t += rng.randrange(1, 3000)
            stamps.append(t)
    rows = []
    for i, ts in enumerate(stamps):
        price = rng.randrange(4_000_000, 4_500_000) / 100
        qty = rng.randrange(1, 50_000) / 1000
        side = rng.randrange(2)
        if rng.random() < 0.15:
            mark = iv = index = None
        else:
            mark = rng.randrange(400, 600) / 10_000
            iv = rng.randrange(3_000, 9_000) / 100
            index = rng.randrange(4_000_000, 4_500_000) / 100
        rows.append(((day_index + 1) * 10_000_000 + i, ts, price, qty, side,
                     mark, iv, index))
    if order == "desc":
        rows.reverse()
    elif order == "shuffled":
        rng.shuffle(rows)
    return rows


def _bybit_uuid(trade_id: int) -> str:
    return str(uuid.UUID(int=random.Random(trade_id).getrandbits(128)))


def _fold(token: str) -> int:
    return int.from_bytes(hashlib.md5(token.encode("ascii")).digest()[:8], "big")


def _opt(v) -> str:
    return "" if v is None else repr(v)


def _zip(dest: Path, text: str) -> Path:
    dest.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(dest.name.rsplit(".", 1)[0] + ".csv", text)
    return dest


def _gz(dest: Path, text: str) -> Path:
    dest.parent.mkdir(parents=True, exist_ok=True)
    with gzip.open(dest, "wt", encoding="utf-8") as f:
        f.write(text)
    return dest


def _lines(header: str, body) -> str:
    buf = io.StringIO()
    buf.write(header + "\n")
    for line in body:
        buf.write(line + "\n")
    return buf.getvalue()


# ── Per-venue layout and the straightforward reading of it ──────────


class _Venue:
    module = None
    symbol = ""
    market = ""
    sorts = True               # stable sort by timestamp before writing
    empty_last_id_is_max = True
    writes_quotes = False

    def write_file(self, dest: Path, rows: list) -> Path:
        raise NotImplementedError

    def tape_id(self, row) -> int:
        return row[0]

    def ts_ns(self, row) -> int:
        return row[1] * 1_000_000

    def side(self, row) -> int:
        return row[4]

    def mirror_path(self, mirror: Path, day: date) -> Path:
        return self.module._mirror_path(mirror, self.symbol, self.market, day)

    def import_file(self, path: Path, tape: Path):
        return self.module.trades_to_lrvx(
            path, tape, symbol_id=_SYMBOL_ID, symbol_name=self.symbol,
            market=self.market)

    def import_range(self, mirror: Path, d0: date, d1: date, tape: Path):
        return self.module.range_to_lrvx(
            self.symbol, self.market, d0, d1, tape, mirror=mirror,
            parallel=1, symbol_id=_SYMBOL_ID)


class _Binance(_Venue):
    module = binance
    symbol = "BTCUSDT"
    market = "um-futures"
    sorts = False

    def side(self, row) -> int:
        return row[4]          # written as is_buyer_maker: true -> SELL (1)

    def write_file(self, dest, rows):
        body = (f"{r[0]},{r[2]!r},{r[3]!r},{r[0]},{r[0]},{r[1]},"
                f"{'true' if r[4] else 'false'},true" for r in rows)
        return _zip(dest, _lines(
            "agg_trade_id,price,quantity,first_trade_id,last_trade_id,"
            "transact_time,is_buyer_maker,is_best_match", body))

    def import_file(self, path, tape):
        return binance.aggtrades_to_lrvx(
            path, tape, symbol_id=_SYMBOL_ID, symbol_name=self.symbol,
            market=self.market)


class _Bybit(_Venue):
    module = bybit
    symbol = "BTCUSDT"
    market = "linear"
    empty_last_id_is_max = False

    def tape_id(self, row) -> int:
        return _fold(_bybit_uuid(row[0]))

    def write_file(self, dest, rows):
        side = ("Buy", "Sell")
        body = (f"{r[1] // 1000}.{r[1] % 1000:03d},BTCUSDT,{side[r[4]]},"
                f"{r[3]!r},{r[2]!r},ZeroPlusTick,{_bybit_uuid(r[0])},0,0,0"
                for r in rows)
        return _gz(dest, _lines(
            "timestamp,symbol,side,size,price,tickDirection,trdMatchID,"
            "grossValue,homeNotional,foreignNotional", body))


class _Okx(_Venue):
    module = okx
    symbol = "BTC-USDT-SWAP"
    market = "swap"

    def write_file(self, dest, rows):
        side = ("buy", "sell")
        body = (f"{r[0]},{side[r[4]]},{r[3]!r},{r[2]!r},{r[1]}" for r in rows)
        return _zip(dest, _lines("trade_id,side,size,price,timestamp_ms", body))


class _Bitget(_Venue):
    module = bitget
    symbol = "BTCUSDT"
    market = "umcbl"

    def write_file(self, dest, rows):
        side = ("buy", "sell")
        body = (f"{r[0]},{r[2]!r},{r[3]!r},{side[r[4]]},{r[1]}" for r in rows)
        return _zip(dest, _lines("trade_id,price,size,side,timestamp_ms", body))


class _Deribit(_Venue):
    module = deribit
    symbol = "BTC-29MAR24-50000-C"
    market = "option"
    writes_quotes = True

    def write_file(self, dest, rows):
        side = ("buy", "sell")
        body = (f"{r[0]},{r[1]},{self.symbol},{side[r[4]]},{r[2]!r},{r[3]!r},"
                f"{_opt(r[5])},{_opt(r[6])},{_opt(r[7])}" for r in rows)
        return _gz(dest, _lines(
            "trade_id,timestamp,instrument_name,direction,price,amount,"
            "mark_price,iv,index_price", body))


def _raw(v: float) -> int:
    v = float(v)
    return int(v * _SCALE + 0.5) if v >= 0.0 else int(v * _SCALE - 0.5)


class _Model:
    """What the tape must hold, from whole-file readings of the CSVs."""

    def __init__(self, venue: _Venue) -> None:
        self.venue = venue
        self.on_tape = []          # (tape_id, ts_ns) of every written trade
        self.segments = []         # expected frames per segment

    def apply(self, rows: list) -> dict:
        v = self.venue
        if not rows:
            return {}
        stamps = [v.ts_ns(r) for r in rows]
        lo, hi = min(stamps), max(stamps)
        present = {i for i, ts in self.on_tape if lo <= ts <= hi}
        kept = [r for r in rows if v.tape_id(r) not in present]
        if v.sorts:
            kept.sort(key=v.ts_ns)
        stats = {
            "rows_read": len(rows),
            "rows_skipped": len(rows) - len(kept),
            "trades_written": len(kept),
        }
        if not kept:
            stats.update(first_ts_ns=0, last_ts_ns=0,
                         last_trade_id=(max(v.tape_id(r) for r in rows)
                                        if v.empty_last_id_is_max else 0))
            return stats
        stats.update(first_ts_ns=v.ts_ns(kept[0]), last_ts_ns=v.ts_ns(kept[-1]),
                     last_trade_id=v.tape_id(kept[-1]))
        frames = [("trade", v.ts_ns(r), v.ts_ns(r), _raw(r[2]), _raw(r[3]),
                   v.tape_id(r), _SYMBOL_ID, v.side(r)) for r in kept]
        if v.writes_quotes:
            for r in kept:
                if r[5] is None and r[6] is None and r[7] is None:
                    continue
                frames.append(("quote", v.ts_ns(r), _raw(r[5] or 0.0),
                               _raw(r[7] or 0.0), int((r[6] or 0.0) * 1e8),
                               _SYMBOL_ID))
        self.segments.append(frames)
        self.on_tape.extend((v.tape_id(r), v.ts_ns(r)) for r in kept)
        return stats


# ── Reading the tape back from its bytes ────────────────────────────


_SEGMENT_HEADER = struct.Struct("<IHBBqqqIIQB15x")
_FRAME_HEADER = struct.Struct("<IIBBH")
_TRADE = struct.Struct("<qqqqQIBBH")
_QUOTE = struct.Struct("<qqqqqqqqqqqqqIBBH")


def _decode_tape(tape: Path) -> list:
    segments = []
    for seg in sorted(tape.glob("*.lrvx")):
        data = seg.read_bytes()
        (_magic, _ver, flags, _ex, _created, _first, _last, count, _syms,
         index_offset, compression) = _SEGMENT_HEADER.unpack_from(data, 0)
        assert compression == 0, "decoder reads uncompressed segments"
        end = index_offset if (flags & 0x01) and index_offset else len(data)
        pos = _SEGMENT_HEADER.size
        frames = []
        while pos < end:
            size, _crc, kind, _rv, _fl = _FRAME_HEADER.unpack_from(data, pos)
            pos += _FRAME_HEADER.size
            if kind == 1:
                t = _TRADE.unpack_from(data, pos)
                frames.append(("trade",) + t[:7])
            elif kind == 4:
                q = _QUOTE.unpack_from(data, pos)
                frames.append(("quote", q[0], q[2], q[3], q[5], q[13]))
            else:
                frames.append(("other", kind))
            pos += size
        assert len(frames) == count
        segments.append(frames)
    return segments


# ── Tests ───────────────────────────────────────────────────────────


class _StreamingImportTests:
    venue: _Venue = None
    batch_rows = 64
    n_rows = 900

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="trade-stream-")
        self.tmp = Path(self._tmp.name)
        self.tape = self.tmp / "tape"
        self.mirror = self.tmp / "mirror"
        self.model = _Model(self.venue)
        patcher = mock.patch.object(_dedup, "_BATCH_ROWS", self.batch_rows,
                                    create=True)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.addCleanup(self._tmp.cleanup)

    def _day(self, i: int, order: str, n: int = 0) -> tuple:
        rows = _make_day(11, i, n or self.n_rows, order)
        path = self.venue.write_file(
            self.venue.mirror_path(self.mirror, _DAY0 + timedelta(days=i)), rows)
        return path, rows

    def _import(self, path: Path, rows: list) -> None:
        stats = self.venue.import_file(path, self.tape)
        expected = self.model.apply(rows)
        for key, value in expected.items():
            self.assertEqual(getattr(stats, key), value, key)
        self.assertEqual(_decode_tape(self.tape), self.model.segments)

    def test_fresh_import_of_an_unordered_day(self) -> None:
        self._import(*self._day(0, "shuffled"))

    def test_fresh_import_of_a_newest_first_day(self) -> None:
        self._import(*self._day(0, "desc"))

    def test_reimport_adds_nothing(self) -> None:
        day = self._day(0, "shuffled")
        self._import(*day)
        self._import(*day)
        self.assertEqual(len(self.model.segments), 1)

    def test_out_of_order_days(self) -> None:
        days = [self._day(0, "asc"), self._day(1, "desc"), self._day(2, "shuffled")]
        for i in (2, 0, 1, 2, 0):
            self._import(*days[i])
        self.assertEqual(len(self.model.segments), 3)

    def test_overlapping_ranges(self) -> None:
        days = [self._day(0, "asc"), self._day(1, "desc"), self._day(2, "shuffled")]
        self.venue.import_range(self.mirror, _DAY0, _DAY0 + timedelta(days=1),
                                self.tape)
        self.model.apply(days[0][1])
        self.model.apply(days[1][1])
        self.venue.import_range(self.mirror, _DAY0 + timedelta(days=1),
                                _DAY0 + timedelta(days=2), self.tape)
        self.model.apply(days[1][1])
        self.model.apply(days[2][1])
        self.assertEqual(_decode_tape(self.tape), self.model.segments)

    def test_partly_present_file_writes_only_the_new_rows(self) -> None:
        _, rows = self._day(0, "asc")
        first = rows[: len(rows) // 2]
        path = self.venue.write_file(self.tmp / ("part." + self._suffix()), first)
        self._import(path, first)
        mixed = list(rows)
        random.Random(5).shuffle(mixed)
        path = self.venue.write_file(self.tmp / ("all." + self._suffix()), mixed)
        self._import(path, mixed)

    def _suffix(self) -> str:
        return "csv.gz" if isinstance(self.venue, (_Bybit, _Deribit)) else "zip"

    def test_peak_memory_is_bounded_by_the_batch(self) -> None:
        # A day of 30k rows held as tuples peaks at 7 MB or more; batches
        # of 2048 rows plus the day's sorted id set stay well under 3 MB.
        with mock.patch.object(_dedup, "_BATCH_ROWS", 2048, create=True):
            path, rows = self._day(0, "shuffled", n=30_000)
            for _ in range(2):
                tracemalloc.start()
                try:
                    self.venue.import_file(path, self.tape)
                    _, peak = tracemalloc.get_traced_memory()
                finally:
                    tracemalloc.stop()
                self.assertLess(peak, 3 * 1024 * 1024)
                self.model.apply(rows)
        self.assertEqual(_decode_tape(self.tape), self.model.segments)


class BinanceStreamingImportTests(_StreamingImportTests, unittest.TestCase):
    venue = _Binance()


class BybitStreamingImportTests(_StreamingImportTests, unittest.TestCase):
    venue = _Bybit()


class OkxStreamingImportTests(_StreamingImportTests, unittest.TestCase):
    venue = _Okx()


class BitgetStreamingImportTests(_StreamingImportTests, unittest.TestCase):
    venue = _Bitget()


class DeribitStreamingImportTests(_StreamingImportTests, unittest.TestCase):
    venue = _Deribit()


class DefaultBatchTests(unittest.TestCase):
    """The default batch size on a day that spans several batches."""

    def test_default_batch_matches_the_model(self) -> None:
        with tempfile.TemporaryDirectory(prefix="trade-stream-") as tmp:
            tmp = Path(tmp)
            venue = _Bybit()
            model = _Model(venue)
            n = getattr(_dedup, "_BATCH_ROWS", 65_536) + 17
            rows = _make_day(3, 0, n, "desc")
            path = venue.write_file(tmp / "BTCUSDT2024-01-15.csv.gz", rows)
            venue.import_file(path, tmp / "tape")
            model.apply(rows)
            self.assertEqual(_decode_tape(tmp / "tape"), model.segments)


if __name__ == "__main__":
    unittest.main()
