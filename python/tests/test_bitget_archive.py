"""Tests for ``lrvx.archives.bitget`` — Bitget public archive converter.

Synthetic CSV matching Bitget's column layout (`trade_id, price,
size, side, timestamp_ms`) is built in memory and round-tripped
through the converter.

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

import lrvx  # noqa: E402

from lrvx.archives import ArchiveReader, bitget  # noqa: E402


# Bitget columns: trade_id, price, size, side, timestamp_ms.
_ROWS = [
    (100, 42_100.5, 0.01, "buy",  1_700_000_000_500),
    (101, 42_101.0, 0.02, "sell", 1_700_000_001_250),
    (102, 42_100.7, 0.03, "buy",  1_700_000_002_000),
]


def _csv_text(rows=_ROWS) -> str:
    buf = io.StringIO()
    buf.write("trade_id,price,size,side,timestamp_ms\n")
    for r in rows:
        buf.write(",".join(str(x) for x in r) + "\n")
    return buf.getvalue()


def _build_zip(dest: Path, rows=_ROWS) -> Path:
    with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(dest.with_suffix(".csv").name, _csv_text(rows))
    return dest


def _build_gz(dest: Path) -> Path:
    with gzip.open(dest, "wt", encoding="utf-8") as f:
        f.write(_csv_text())
    return dest


class BitgetConvertTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="bitget-"))
        self.tape = self.tmp / "tape"

    def _convert_zip(self) -> bitget.ConvertStats:
        z = self.tmp / "BTCUSDT-trades-2024-01-15.zip"
        _build_zip(z)
        return bitget.trades_to_lrvx(
            z, self.tape,
            symbol_id=7, symbol_name="BTCUSDT", market="umcbl",
        )

    def test_round_trip_zip(self) -> None:
        stats = self._convert_zip()
        self.assertEqual(stats.trades_written, 3)
        trades = lrvx.DataReader(str(self.tape)).read_trades()
        self.assertEqual(int(trades.size), 3)
        sides = [int(t["side"]) for t in trades]
        self.assertEqual(sides, [0, 1, 0])
        ts_ns = [int(t["exchange_ts_ns"]) for t in trades]
        self.assertEqual(ts_ns,
                         [1_700_000_000_500_000_000,
                          1_700_000_001_250_000_000,
                          1_700_000_002_000_000_000])

    def test_round_trip_gz(self) -> None:
        g = self.tmp / "BTCUSDT-trades-2024-01-15.csv.gz"
        _build_gz(g)
        stats = bitget.trades_to_lrvx(
            g, self.tape,
            symbol_id=7, symbol_name="BTCUSDT", market="umcbl",
        )
        self.assertEqual(stats.trades_written, 3)

    def test_dedup_on_rerun(self) -> None:
        first = self._convert_zip()
        self.assertEqual(first.trades_written, 3)
        second = bitget.trades_to_lrvx(
            self.tmp / "BTCUSDT-trades-2024-01-15.zip", self.tape,
            symbol_id=7, symbol_name="BTCUSDT", market="umcbl",
        )
        self.assertEqual(second.trades_written, 0)
        self.assertEqual(second.rows_skipped, 3)

    def test_metadata(self) -> None:
        self._convert_zip()
        meta = json.loads((self.tape / "metadata.json").read_text())
        self.assertEqual(meta["exchange"], "bitget")
        self.assertEqual(meta["instrument_type"], "perpetual")
        self.assertEqual(meta["total_trades"], 3)
        sym = meta["symbols"][0]
        self.assertEqual(sym["name"], "BTCUSDT")
        self.assertEqual(sym["base_asset"], "BTC")
        self.assertEqual(sym["quote_asset"], "USDT")

    def test_spot_market_metadata(self) -> None:
        z = self.tmp / "BTCUSDT-trades-2024-01-15.zip"
        _build_zip(z)
        bitget.trades_to_lrvx(
            z, self.tape, symbol_id=7,
            symbol_name="BTCUSDT", market="spot",
        )
        meta = json.loads((self.tape / "metadata.json").read_text())
        self.assertEqual(meta["instrument_type"], "spot")

    def test_protocol_compliance(self) -> None:
        self.assertIsInstance(bitget, ArchiveReader)


_DAY1_START_MS = 1_705_276_800_000  # 2024-01-15T00:00:00Z
_DAY_MS = 86_400_000
_ROWS_PER_DAY = 40


def _day_rows(day_index: int) -> list:
    """Bitget rows for one synthetic day. Ids increase with time
    across days, as on the exchange."""
    start = _DAY1_START_MS + day_index * _DAY_MS
    base = 50_000 + day_index * _ROWS_PER_DAY
    return [(base + i, 42_000.0 + i, 0.01 * (i + 1),
             "buy" if i % 2 == 0 else "sell", start + i * 60_000)
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
            dest = bitget._mirror_path(self.mirror, "BTCUSDT", "umcbl",
                                       date(2024, 1, 15 + i))
            dest.parent.mkdir(parents=True, exist_ok=True)
            self.paths.append(_build_zip(dest, rows))

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
        ts = [r[4] * 1_000_000 for r in self.days[1]]
        for kwargs in calls:
            self.assertEqual(kwargs.get("from_ns"), min(ts))
            self.assertEqual(kwargs.get("to_ns"), max(ts))
            self.assertEqual(kwargs.get("symbols"), [7])


if __name__ == "__main__":
    unittest.main()
