"""Tests for ``lrvx.archives.deribit`` — Deribit public archive
converter (perpetual / dated future / option instruments).

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_deribit_archive.py
"""
from __future__ import annotations

import gzip
import io
import json
import sys
import tempfile
import unittest
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

from lrvx.archives import ArchiveReader, deribit  # noqa: E402


# Deribit columns: trade_id, timestamp_ms, instrument, side, price,
# amount, mark_price, iv, index_price.
_ROWS_PERP = [
    (100, 1_700_000_000_500, "BTC-PERPETUAL", "buy",  42_100.5, 0.1, 42_100.0, 0.0, 42_098.0),
    (101, 1_700_000_001_250, "BTC-PERPETUAL", "sell", 42_101.0, 0.2, 42_101.0, 0.0, 42_099.5),
    (102, 1_700_000_002_000, "BTC-PERPETUAL", "buy",  42_100.7, 0.3, 42_100.7, 0.0, 42_098.5),
]

_ROWS_OPTION = [
    (200, 1_700_000_000_000, "BTC-29MAR24-50000-C", "buy",  0.0500, 10.0, 0.0498, 0.55, 42_000.0),
    (201, 1_700_000_001_000, "BTC-29MAR24-50000-C", "sell", 0.0510, 5.0,  0.0511, 0.56, 42_010.0),
]


def _csv_text(rows) -> str:
    buf = io.StringIO()
    buf.write("trade_id,timestamp_ms,instrument,side,price,amount,"
              "mark_price,iv,index_price\n")
    for r in rows:
        buf.write(",".join(str(x) for x in r) + "\n")
    return buf.getvalue()


def _build_gz(dest: Path, rows) -> Path:
    with gzip.open(dest, "wt", encoding="utf-8") as f:
        f.write(_csv_text(rows))
    return dest


class DeribitPerpetualTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="deribit-"))
        self.tape = self.tmp / "tape"

    def _convert(self) -> deribit.ConvertStats:
        g = self.tmp / "BTC-PERPETUAL-2024-01-15.csv.gz"
        _build_gz(g, _ROWS_PERP)
        return deribit.trades_to_lrvx(
            g, self.tape,
            symbol_id=7, symbol_name="BTC-PERPETUAL", market="perpetual",
        )

    def test_round_trip(self) -> None:
        stats = self._convert()
        self.assertEqual(stats.trades_written, 3)
        trades = lrvx.DataReader(str(self.tape)).read_trades()
        self.assertEqual(int(trades.size), 3)
        sides = [int(t["side"]) for t in trades]
        self.assertEqual(sides, [0, 1, 0])

    def test_dedup_on_rerun(self) -> None:
        first = self._convert()
        self.assertEqual(first.trades_written, 3)
        second = deribit.trades_to_lrvx(
            self.tmp / "BTC-PERPETUAL-2024-01-15.csv.gz", self.tape,
            symbol_id=7, symbol_name="BTC-PERPETUAL", market="perpetual",
        )
        self.assertEqual(second.trades_written, 0)
        self.assertEqual(second.rows_skipped, 3)

    def test_metadata(self) -> None:
        self._convert()
        meta = json.loads((self.tape / "metadata.json").read_text())
        self.assertEqual(meta["exchange"], "deribit")
        self.assertEqual(meta["instrument_type"], "perpetual")
        self.assertEqual(meta["total_trades"], 3)
        sym = meta["symbols"][0]
        self.assertEqual(sym["name"], "BTC-PERPETUAL")
        self.assertEqual(sym["base_asset"], "BTC")
        self.assertEqual(sym["quote_asset"], "USD")


class DeribitOptionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="deribit-opt-"))
        self.tape = self.tmp / "tape"

    def test_option_metadata(self) -> None:
        g = self.tmp / "BTC-29MAR24-50000-C-2024-01-15.csv.gz"
        _build_gz(g, _ROWS_OPTION)
        stats = deribit.trades_to_lrvx(
            g, self.tape, symbol_id=7,
            symbol_name="BTC-29MAR24-50000-C", market="option",
        )
        self.assertEqual(stats.trades_written, 2)
        meta = json.loads((self.tape / "metadata.json").read_text())
        self.assertEqual(meta["instrument_type"], "option")
        sym = meta["symbols"][0]
        self.assertEqual(sym["name"], "BTC-29MAR24-50000-C")
        self.assertEqual(sym["base_asset"], "BTC")

    def test_option_quotes_preserved(self) -> None:
        # mark/iv/index from the option CSV are kept as OptionQuote frames
        # instead of dropped, and read back via read_option_quotes_from.
        g = self.tmp / "BTC-29MAR24-50000-C-2024-01-15.csv.gz"
        _build_gz(g, _ROWS_OPTION)
        deribit.trades_to_lrvx(
            g, self.tape, symbol_id=7,
            symbol_name="BTC-29MAR24-50000-C", market="option",
        )
        reader = lrvx.DataReader(str(self.tape))
        quotes = reader.read_option_quotes_from(0)
        self.assertEqual(quotes.size, 2)
        # Source row 200: mark=0.0498, iv=0.55, index=42000.0
        self.assertAlmostEqual(quotes["mark_price_raw"][0] / 1e8, 0.0498, places=6)
        self.assertAlmostEqual(quotes["iv_raw"][0] / 1e8, 0.55, places=6)
        self.assertAlmostEqual(quotes["index_price_raw"][0] / 1e8, 42_000.0, places=2)
        self.assertEqual(int(quotes["symbol_id"][0]), 7)
        # Trades are still written alongside the quotes.
        self.assertEqual(reader.read_trades().size, 2)

    def test_perpetual_emits_no_option_quotes(self) -> None:
        g = self.tmp / "BTC-PERPETUAL-2024-01-15.csv.gz"
        _build_gz(g, _ROWS_PERP)
        deribit.trades_to_lrvx(
            g, self.tape, symbol_id=7, symbol_name="BTC-PERPETUAL", market="perpetual",
        )
        reader = lrvx.DataReader(str(self.tape))
        self.assertEqual(reader.read_option_quotes_from(0).size, 0)

    def test_protocol_compliance(self) -> None:
        self.assertIsInstance(deribit, ArchiveReader)


_DAY1_START_MS = 1_705_276_800_000  # 2024-01-15T00:00:00Z
_DAY_MS = 86_400_000
_ROWS_PER_DAY = 40


def _day_rows(day_index: int) -> list:
    """Deribit rows for one synthetic day. Ids increase with time
    across days, as on the exchange."""
    start = _DAY1_START_MS + day_index * _DAY_MS
    base = 50_000 + day_index * _ROWS_PER_DAY
    return [(base + i, start + i * 60_000, "BTC-PERPETUAL",
             "buy" if i % 2 == 0 else "sell", 42_000.0 + i, 0.1 * (i + 1),
             42_000.0 + i, 0.0, 41_990.0 + i)
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


class DeribitMultiDayAppendTests(unittest.TestCase):
    """Append dedup across days: every day lands once, whatever order
    the days are imported in, and the dedup read covers only the
    incoming day's time range."""

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="deribit-days-"))
        self.mirror = self.tmp / "mirror"
        self.tape = self.tmp / "tape"
        self.days = [_day_rows(i) for i in range(3)]
        self.paths = []
        for i, rows in enumerate(self.days):
            dest = deribit._mirror_path(self.mirror, "BTC-PERPETUAL", "perpetual",
                                        date(2024, 1, 15 + i))
            dest.parent.mkdir(parents=True, exist_ok=True)
            self.paths.append(_build_gz(dest, rows))

    def _import(self, day_index: int) -> deribit.ConvertStats:
        return deribit.trades_to_lrvx(
            self.paths[day_index], self.tape, symbol_id=7,
            symbol_name="BTC-PERPETUAL", market="perpetual",
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
        deribit.range_to_lrvx(
            "BTC-PERPETUAL", "perpetual", "2024-01-15", "2024-01-16", self.tape,
            mirror=self.mirror, parallel=1, symbol_id=7,
        )
        stats = deribit.range_to_lrvx(
            "BTC-PERPETUAL", "perpetual", "2024-01-16", "2024-01-17", self.tape,
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


if __name__ == "__main__":
    unittest.main()
