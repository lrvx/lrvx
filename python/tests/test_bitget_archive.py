"""Tests for ``lrvx.archives.bitget`` — Bitget public archive converter.

CSV in the layout Bitget publishes (`trade_id,timestamp,price,side,
volume(quote),size(base)`, header and row shapes copied from real
files) is built in memory and round-tripped through the converter.

Covers the round trip, the part URLs and the three part namings a day
may use, a multi-part day, rejection of an unknown header, the
nothing-parsed error, dedup across days in any order and, with
LRVX_ARCHIVE_NETWORK_TESTS=1, real days downloaded from Bitget.

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_bitget_archive.py
"""
from __future__ import annotations

import gzip
import io
import json
import sys
import tempfile
import unittest
import zipfile
from datetime import date
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[2]
for cand in ("build/python", "build-py312/python"):
    p = REPO_ROOT / cand
    if p.is_dir():
        sys.path.insert(0, str(p))
        break
sys.path.insert(0, str(Path(__file__).resolve().parent))

import lrvx  # noqa: E402

import _archive_network as net  # noqa: E402
from lrvx.archives import ArchiveReader, bitget  # noqa: E402

_HEADER = "trade_id,timestamp,price,side,volume(quote),size(base)"

# (trade_id, timestamp_ms, price, side, size_base), shaped like
# BTCUSDT USDT-margined rows of 2026-11-09.
_ROWS = [
    (1503354649689448450, 1_794_153_600_000, 75943.5, "buy", 0.0001),
    (1503354771215212544, 1_794_153_629_000, 75941.9, "sell", 0.3368),
    (1503354771215212546, 1_794_153_629_000, 75941.8, "buy", 0.0095),
]


def _csv_text(rows=_ROWS) -> str:
    buf = io.StringIO()
    buf.write(_HEADER + "\n")
    for t, ts, p, s, q in rows:
        buf.write(f"{t},{ts},{p},{s},{p * q!r},{q}\n")
    return buf.getvalue()


def _build_zip(dest: Path, rows=_ROWS, text: str = "") -> Path:
    dest.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(dest.with_suffix(".csv").name, text or _csv_text(rows))
    return dest


def _build_gz(dest: Path) -> Path:
    with gzip.open(dest, "wt", encoding="utf-8") as f:
        f.write(_csv_text())
    return dest


def _trades(tape: Path) -> list:
    t = lrvx.DataReader(str(tape)).read_trades()
    return [(int(t["trade_id"][i]), int(t["exchange_ts_ns"][i]),
             int(t["price_raw"][i]), int(t["side"][i]), int(t["qty_raw"][i]))
            for i in range(t.size)]


def _expected(rows=_ROWS) -> list:
    return [(t, ts * 1_000_000, round(p * 1e8), 0 if s == "buy" else 1,
             round(q * 1e8)) for t, ts, p, s, q in rows]


class BitgetConvertTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="bitget-"))
        self.tape = self.tmp / "tape"
        self.zip_path = self.tmp / "BTCUSDT_UMCBL_20261109_001.zip"

    def _convert_zip(self) -> bitget.ConvertStats:
        _build_zip(self.zip_path)
        return bitget.trades_to_lrvx(
            self.zip_path, self.tape, symbol_id=7, market="umcbl",
        )

    def test_round_trip_zip(self) -> None:
        stats = self._convert_zip()
        self.assertEqual(stats.trades_written, 3)
        self.assertEqual(_trades(self.tape), _expected())

    def test_round_trip_gz(self) -> None:
        g = self.tmp / "BTCUSDT_20261109_001.csv.gz"
        _build_gz(g)
        stats = bitget.trades_to_lrvx(g, self.tape, symbol_id=7, market="umcbl")
        self.assertEqual(stats.trades_written, 3)

    def test_symbol_from_file_name(self) -> None:
        self._convert_zip()
        meta = json.loads((self.tape / "metadata.json").read_text())
        self.assertEqual(meta["symbols"][0]["name"], "BTCUSDT")

    def test_dated_part_name_needs_a_symbol(self) -> None:
        z = _build_zip(self.tmp / "20250115_001.zip")
        with self.assertRaises(ValueError):
            bitget.trades_to_lrvx(z, self.tape, market="umcbl")
        stats = bitget.trades_to_lrvx(z, self.tape, market="umcbl",
                                      symbol_name="BTCUSDT")
        self.assertEqual(stats.trades_written, 3)

    def test_dedup_on_rerun(self) -> None:
        first = self._convert_zip()
        self.assertEqual(first.trades_written, 3)
        second = bitget.trades_to_lrvx(
            self.zip_path, self.tape, symbol_id=7, market="umcbl",
        )
        self.assertEqual(second.trades_written, 0)
        self.assertEqual(second.rows_skipped, 3)

    def test_unknown_header_is_rejected(self) -> None:
        z = _build_zip(self.zip_path, text="trade_id,price,size,side,timestamp_ms\n"
                                           "100,42100.5,0.01,buy,1700000000500\n")
        with self.assertRaises(bitget.UnknownLayoutError) as ctx:
            bitget.trades_to_lrvx(z, self.tape, market="umcbl")
        self.assertIn(_HEADER, str(ctx.exception))
        self.assertFalse(self.tape.exists())

    def test_nothing_parsed_is_an_error(self) -> None:
        z = _build_zip(self.zip_path, text=_HEADER + "\n"
                       "1,1794153600000,75943.5,hold,7.59,0.0001\n"
                       "2,soon,75943.5,buy,7.59,0.0001\n")
        with self.assertRaises(bitget.NothingParsedError):
            bitget.trades_to_lrvx(z, self.tape, market="umcbl")
        self.assertFalse(self.tape.exists())

    def test_metadata(self) -> None:
        self._convert_zip()
        meta = json.loads((self.tape / "metadata.json").read_text())
        self.assertEqual(meta["exchange"], "bitget")
        self.assertEqual(meta["instrument_type"], "perpetual")
        self.assertEqual(meta["total_trades"], 3)
        self.assertEqual(meta["custom"], {"qty_unit": "base"})
        sym = meta["symbols"][0]
        self.assertEqual(sym["name"], "BTCUSDT")
        self.assertEqual(sym["base_asset"], "BTC")
        self.assertEqual(sym["quote_asset"], "USDT")

    def test_market_metadata(self) -> None:
        for market, symbol, quote in (("spot", "BTCUSDT", "USDT"),
                                      ("cmcbl", "BTCPERP", "USDC"),
                                      ("dmcbl", "BTCUSD", "USD")):
            tape = self.tmp / market
            z = _build_zip(self.tmp / market / f"{symbol}_20261109_001.zip")
            bitget.trades_to_lrvx(z, tape / "t", market=market)
            meta = json.loads((tape / "t" / "metadata.json").read_text())
            self.assertEqual(meta["instrument_type"],
                             "spot" if market == "spot" else "perpetual")
            self.assertEqual(meta["symbols"][0]["quote_asset"], quote)

    def test_unknown_market(self) -> None:
        _build_zip(self.zip_path)
        with self.assertRaises(ValueError):
            bitget.trades_to_lrvx(self.zip_path, self.tape, market="usdt")

    def test_protocol_compliance(self) -> None:
        self.assertIsInstance(bitget, ArchiveReader)


class BitgetPartsTests(unittest.TestCase):
    """A day is fetched part by part, in whichever naming it uses."""

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="bitget-parts-"))
        self.mirror = self.tmp / "mirror"
        self.day = date(2025, 1, 15)

    def test_part_urls(self) -> None:
        base = "https://img.bitgetimg.com/online/trades/"
        urls = [bitget._part_url("BTCUSDT", "umcbl", self.day, pat, 2)
                for pat in bitget._PART_NAMES]
        self.assertEqual(urls, [
            base + "UMCBL/BTCUSDT/BTCUSDT_UMCBL_20250115_002.zip",
            base + "UMCBL/BTCUSDT/20250115_002.zip",
            base + "UMCBL/BTCUSDT/BTCUSDT_20250115_002.zip",
        ])
        self.assertEqual(bitget._part_url("BTC", "spot", self.day,
                                          bitget._PART_NAMES[0], 1),
                         base + "SPBL/BTC/BTC_SPBL_20250115_001.zip")
        self.assertIn("/CMCBL/BTCPERP/", bitget._part_url(
            "BTCPERP", "cmcbl", self.day, bitget._PART_NAMES[2], 1))
        self.assertIn("/DMCBL/BTCUSD/", bitget._part_url(
            "BTCUSD", "dmcbl", self.day, bitget._PART_NAMES[2], 1))

    def _serve(self, published: dict):
        """Patch ``_fetch`` to serve the URLs in ``published`` (URL →
        rows) and answer missing for any other; returns the patcher and
        the list of URLs requested."""
        asked: list = []

        def fetch(url, dest, **_kw):
            asked.append(url)
            if url not in published:
                return False
            _build_zip(dest, published[url])
            return True

        return mock.patch.object(bitget, "_fetch", fetch), asked

    def test_day_in_second_naming_with_two_parts(self) -> None:
        pat = bitget._PART_NAMES[1]
        part1, part2 = _ROWS[:2], _ROWS[2:]
        published = {
            bitget._part_url("BTCUSDT", "umcbl", self.day, pat, 1): part1,
            bitget._part_url("BTCUSDT", "umcbl", self.day, pat, 2): part2,
        }
        patcher, asked = self._serve(published)
        tape = self.tmp / "tape"
        with patcher:
            stats = bitget.range_to_lrvx("BTCUSDT", "umcbl", self.day, self.day,
                                         tape, mirror=self.mirror, parallel=1)
        self.assertEqual((stats.trades_written, stats.files_processed), (3, 2))
        self.assertEqual(_trades(tape), _expected())
        # First naming tried and missing, then two parts and the miss
        # that ends the day.
        self.assertEqual(len(asked), 4)
        with patcher:
            again = bitget.range_to_lrvx("BTCUSDT", "umcbl", self.day, self.day,
                                         tape, mirror=self.mirror, parallel=1)
        self.assertEqual(len(asked), 4)     # served from the mirror manifest
        self.assertEqual((again.trades_written, again.rows_skipped), (0, 3))

    def test_missing_day(self) -> None:
        patcher, asked = self._serve({})
        with patcher:
            with self.assertRaises(FileNotFoundError):
                bitget.range_to_lrvx("BTCUSDT", "umcbl", self.day, self.day,
                                     self.tmp / "tape", mirror=self.mirror)
            stats = bitget.range_to_lrvx("BTCUSDT", "umcbl", self.day, self.day,
                                         self.tmp / "tape", mirror=self.mirror,
                                         skip_missing=True)
        self.assertEqual(stats.files_processed, 0)
        self.assertEqual(len(asked), 6)


_DAY1_START_MS = 1_705_248_000_000  # 2024-01-15T00:00:00+08:00
_DAY_MS = 86_400_000
_ROWS_PER_DAY = 40


def _day_rows(day_index: int) -> list:
    """Bitget rows for one synthetic day. Ids increase with time
    across days, as on the exchange."""
    start = _DAY1_START_MS + day_index * _DAY_MS
    base = 1_130_457_536_508_936_193 + day_index * _ROWS_PER_DAY
    return [(base + i, start + i * 60_000, 42_000.0 + i,
             "buy" if i % 2 == 0 else "sell", 0.01 * (i + 1))
            for i in range(_ROWS_PER_DAY)]


def _record_reader_ranges():
    """Patch ``lrvx.DataReader`` to record the keyword arguments of each
    construction; returns the patcher and the list it fills."""
    calls: list = []
    real = lrvx.DataReader

    def _wrapped(*args, **kwargs):
        calls.append(kwargs)
        return real(*args, **kwargs)

    return mock.patch.object(lrvx, "DataReader", _wrapped), calls


class BitgetMultiDayAppendTests(unittest.TestCase):
    """Append dedup across days: every day lands once, whatever order
    the days are imported in, and the dedup read covers only the
    incoming day's time range."""

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="bitget-days-"))
        self.mirror = self.tmp / "mirror"
        self.tape = self.tmp / "tape"
        self.days = [_day_rows(i) for i in range(3)]
        self.paths = []
        for i, rows in enumerate(self.days):
            day = date(2024, 1, 15 + i)
            dest = bitget._mirror_path(self.mirror, "BTCUSDT", "umcbl", day)
            self.paths.append(_build_zip(dest, rows))
            bitget._manifest_path(self.mirror, "BTCUSDT", "umcbl", day) \
                .write_text(dest.name + "\n")

    def _import(self, day_index: int) -> bitget.ConvertStats:
        return bitget.trades_to_lrvx(
            self.paths[day_index], self.tape, symbol_id=7,
            symbol_name="BTCUSDT", market="umcbl",
        )

    def _tape_ids(self) -> list:
        trades = lrvx.DataReader(str(self.tape)).read_trades()
        return [int(t["trade_id"]) for t in trades]

    def _expected_ids(self, *day_indexes: int) -> list:
        return sorted(r[0] for i in day_indexes for r in self.days[i])

    def test_two_days_in_sequence_both_land_in_full(self) -> None:
        first = self._import(0)
        second = self._import(1)
        self.assertEqual(first.trades_written, _ROWS_PER_DAY)
        self.assertEqual(second.trades_written, _ROWS_PER_DAY)
        self.assertEqual(second.rows_skipped, 0)
        self.assertEqual(sorted(self._tape_ids()), self._expected_ids(0, 1))

    def test_reimport_of_same_day_adds_nothing(self) -> None:
        self._import(0)
        self._import(1)
        for day in (1, 0):
            again = self._import(day)
            self.assertEqual(again.trades_written, 0)
            self.assertEqual(again.rows_skipped, _ROWS_PER_DAY)
        self.assertEqual(sorted(self._tape_ids()), self._expected_ids(0, 1))

    def test_overlapping_range_keeps_each_trade_once(self) -> None:
        bitget.range_to_lrvx(
            "BTCUSDT", "umcbl", "2024-01-15", "2024-01-16", self.tape,
            mirror=self.mirror, parallel=1, symbol_id=7,
        )
        stats = bitget.range_to_lrvx(
            "BTCUSDT", "umcbl", "2024-01-16", "2024-01-17", self.tape,
            mirror=self.mirror, parallel=1, symbol_id=7,
        )
        self.assertEqual(stats.trades_written, _ROWS_PER_DAY)
        self.assertEqual(stats.rows_skipped, _ROWS_PER_DAY)
        ids = self._tape_ids()
        self.assertEqual(len(ids), len(set(ids)))
        self.assertEqual(sorted(ids), self._expected_ids(0, 1, 2))

    def test_out_of_order_days_lose_nothing(self) -> None:
        later = self._import(2)
        earlier = self._import(0)
        self.assertEqual(later.trades_written, _ROWS_PER_DAY)
        self.assertEqual(earlier.trades_written, _ROWS_PER_DAY)
        self.assertEqual(earlier.rows_skipped, 0)
        middle = self._import(1)
        self.assertEqual(middle.trades_written, _ROWS_PER_DAY)
        for day in (2, 0):
            self.assertEqual(self._import(day).trades_written, 0)
        self.assertEqual(sorted(self._tape_ids()), self._expected_ids(0, 1, 2))

    def test_dedup_read_is_bounded_to_the_incoming_day(self) -> None:
        self._import(0)
        patcher, calls = _record_reader_ranges()
        with patcher:
            self._import(1)
        self.assertTrue(calls)
        ts = [r[1] * 1_000_000 for r in self.days[1]]
        for kwargs in calls:
            self.assertEqual(kwargs.get("from_ns"), min(ts))
            self.assertEqual(kwargs.get("to_ns"), max(ts))
            self.assertEqual(kwargs.get("symbols"), [7])


def _decode_bitget(row):
    return (int(row[0]), float(row[2]), float(row[5]), int(row[1]) * 1_000_000,
            0 if row[3] == "buy" else 1)


@unittest.skipUnless(net.ENABLED, net.SKIP_REASON)
class BitgetRealArchiveTests(unittest.TestCase):
    """One real day per market, downloaded from Bitget's CDN; the days
    cover the three part namings."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="bitget-net-")
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)

    def _check(self, symbol: str, market: str, day: str) -> None:
        tape = self.tmp / f"{market}-tape"
        mirror = self.tmp / "mirror"
        stats = bitget.range_to_lrvx(symbol, market, day, day, tape,
                                     mirror=mirror, parallel=1)
        self.assertGreater(stats.trades_written, 0)
        self.assertEqual(stats.rows_invalid, 0)
        manifest = bitget._manifest_path(mirror, symbol, market,
                                         date.fromisoformat(day))
        expected = []
        for name in manifest.read_text().split():
            expected += net.expected_from(manifest.parent / name, _decode_bitget)
        net.assert_sample(self, tape, expected)

    def test_umcbl(self) -> None:
        self._check("DOGEUSDT", "umcbl", "2026-11-09")

    def test_spot(self) -> None:
        self._check("BTCUSDT", "spot", "2022-01-15")

    def test_cmcbl(self) -> None:
        self._check("BTCPERP", "cmcbl", "2026-11-09")

    def test_dmcbl(self) -> None:
        self._check("BTCUSD", "dmcbl", "2025-01-15")


if __name__ == "__main__":
    unittest.main()
