"""Tests for ``lrvx.archives.binance`` — Binance public aggTrades
zip → lrvx converter.

No network: a synthetic ``aggTrades.zip`` matching the exact column
layout published on ``data.binance.vision`` is built in-memory and
fed through the converter. The resulting `.lrvx` directory is
read back via ``lrvx.DataReader`` and asserted trade-by-trade.

Covers:

  * Single-day conversion via ``aggtrades_to_lrvx``.
  * Side mapping: ``is_buyer_maker=True`` → ``Side::SELL``.
  * Header-row autoskip (older daily files prepend a header).
  * Append + dedup: re-running the same day writes zero new trades.
  * Multi-day append: days in sequence, re-imports, overlapping
    ranges and out-of-order days each keep every trade once, and the
    dedup read is bounded to the incoming day's time range.
  * ``metadata.json`` is produced with exchange / instrument_type /
    symbol entry / time range.
  * CLI wiring: ``lrvx archive binance --csv ... --out ...``.

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_binance_archive.py
"""
from __future__ import annotations

import io
import json
import sys
import tempfile
import unittest
import zipfile
from contextlib import redirect_stdout
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

from lrvx import cli  # noqa: E402
from lrvx.archives import binance as bnc  # noqa: E402


# Synthetic rows mirroring the public archive layout. The third column
# of each tuple is is_buyer_maker, which the converter must map to
# Side::SELL (1) when true and Side::BUY (0) when false.
_ROWS = [
    # (agg_id, price, qty, first_id, last_id, ts_ms, is_buyer_maker, is_best)
    (1001, 42100.50, 0.005, 9001, 9001, 1_700_000_000_000, "True",  "True"),
    (1002, 42101.00, 0.010, 9002, 9003, 1_700_000_001_000, "False", "True"),
    (1003, 42100.75, 0.002, 9004, 9004, 1_700_000_002_500, "True",  "True"),
    (1004, 42102.25, 0.020, 9005, 9006, 1_700_000_003_750, "False", "True"),
]


def _build_zip(dest: Path, *, with_header: bool, rows=_ROWS) -> Path:
    """Pack ``rows`` into a Binance-style aggTrades zip."""
    buf = io.StringIO()
    if with_header:
        buf.write("agg_trade_id,price,quantity,first_trade_id,last_trade_id,"
                  "transact_time,is_buyer_maker,is_best_match\n")
    for r in rows:
        buf.write(",".join(str(x) for x in r) + "\n")
    csv_bytes = buf.getvalue().encode("ascii")
    with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(dest.with_suffix(".csv").name, csv_bytes)
    return dest


class SingleDayConvertTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="binance-archive-"))
        self.zip_path = self.tmp / "BTCUSDT-aggTrades-2024-01-15.zip"
        self.tape = self.tmp / "tape"

    def _convert(self, *, with_header: bool, append=True) -> bnc.ConvertStats:
        _build_zip(self.zip_path, with_header=with_header)
        return bnc.aggtrades_to_lrvx(
            self.zip_path,
            self.tape,
            symbol_id=7,
            symbol_name="BTCUSDT",
            market="um-futures",
            append=append,
        )

    def test_round_trip_with_header(self) -> None:
        stats = self._convert(with_header=True)
        self.assertEqual(stats.trades_written, 4)
        self.assertEqual(stats.rows_read, 4)
        self.assertEqual(stats.rows_skipped, 0)
        self._assert_tape_matches()

    def test_round_trip_no_header(self) -> None:
        stats = self._convert(with_header=False)
        self.assertEqual(stats.trades_written, 4)
        self._assert_tape_matches()

    def test_side_mapping(self) -> None:
        self._convert(with_header=False)
        trades = lrvx.DataReader(str(self.tape)).read_trades()
        # is_buyer_maker=True → SELL (1); False → BUY (0).
        sides = [int(t["side"]) for t in trades]
        self.assertEqual(sides, [1, 0, 1, 0])

    def test_append_is_dedup(self) -> None:
        first = self._convert(with_header=False)
        self.assertEqual(first.trades_written, 4)
        # Re-run the same day. Every row's agg_id is ≤ last_trade_id,
        # so nothing new lands in the tape.
        second = bnc.aggtrades_to_lrvx(
            self.zip_path, self.tape,
            symbol_id=7, symbol_name="BTCUSDT", market="um-futures",
            append=True,
        )
        self.assertEqual(second.trades_written, 0)
        self.assertEqual(second.rows_skipped, 4)
        trades = lrvx.DataReader(str(self.tape)).read_trades()
        self.assertEqual(int(trades.size), 4)

    def test_metadata_emitted(self) -> None:
        self._convert(with_header=False)
        meta = json.loads((self.tape / "metadata.json").read_text())
        self.assertEqual(meta["exchange"], "binance")
        self.assertEqual(meta["instrument_type"], "perpetual")
        self.assertEqual(meta["total_trades"], 4)
        self.assertTrue(meta["has_trades"])
        self.assertFalse(meta["has_book_snapshots"])
        self.assertEqual(len(meta["symbols"]), 1)
        sym = meta["symbols"][0]
        self.assertEqual(sym["symbol_id"], 7)
        self.assertEqual(sym["name"], "BTCUSDT")
        self.assertEqual(sym["base_asset"], "BTC")
        self.assertEqual(sym["quote_asset"], "USDT")

    def test_cli_wiring(self) -> None:
        _build_zip(self.zip_path, with_header=False)
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = cli.main([
                "archive", "binance",
                "--csv", str(self.zip_path),
                "--out", str(self.tape),
                "--symbol-id", "7",
                "--symbol", "BTCUSDT",
                "--market", "um-futures",
            ])
        self.assertEqual(rc, 0, msg=buf.getvalue())
        out = buf.getvalue()
        self.assertIn("trades_written=4", out)
        trades = lrvx.DataReader(str(self.tape)).read_trades()
        self.assertEqual(int(trades.size), 4)

    def _assert_tape_matches(self) -> None:
        trades = lrvx.DataReader(str(self.tape)).read_trades()
        self.assertEqual(int(trades.size), 4)
        # Trade IDs round-trip as agg_trade_id.
        self.assertEqual([int(t["trade_id"]) for t in trades],
                         [1001, 1002, 1003, 1004])
        # Timestamps round-trip as ms → ns.
        self.assertEqual([int(t["exchange_ts_ns"]) for t in trades],
                         [r[5] * 1_000_000 for r in _ROWS])
        # Symbol IDs match the converter's argument.
        for t in trades:
            self.assertEqual(int(t["symbol_id"]), 7)
        # Prices round-trip within the 1e-8 fixed-point grid.
        for t, r in zip(trades, _ROWS):
            self.assertAlmostEqual(float(t["price_raw"]) / 1e8, r[1], places=6)
            self.assertAlmostEqual(float(t["qty_raw"]) / 1e8, r[2], places=6)


_DAY1_START_MS = 1_705_276_800_000  # 2024-01-15T00:00:00Z
_DAY_MS = 86_400_000
_ROWS_PER_DAY = 40


def _day_rows(day_index: int) -> list:
    """aggTrades rows for one synthetic day. Ids increase with time
    across days, as on the exchange."""
    start = _DAY1_START_MS + day_index * _DAY_MS
    base = 50_000 + day_index * _ROWS_PER_DAY
    return [(base + i, 42_000.0 + i, 0.01 * (i + 1), base + i, base + i,
             start + i * 60_000, "True" if i % 2 else "False", "True")
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


class MultiDayAppendTests(unittest.TestCase):
    """Append dedup across days: every day lands once, whatever order
    the days are imported in, and the dedup read covers only the
    incoming day's time range."""

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="binance-days-"))
        self.mirror = self.tmp / "mirror"
        self.tape = self.tmp / "tape"
        self.days = [_day_rows(i) for i in range(3)]
        self.paths = []
        for i, rows in enumerate(self.days):
            dest = bnc._mirror_path(self.mirror, "BTCUSDT", "um-futures",
                                    date(2024, 1, 15 + i))
            dest.parent.mkdir(parents=True, exist_ok=True)
            self.paths.append(_build_zip(dest, with_header=False, rows=rows))

    def _import(self, day_index: int) -> bnc.ConvertStats:
        return bnc.aggtrades_to_lrvx(
            self.paths[day_index], self.tape, symbol_id=7,
            symbol_name="BTCUSDT", market="um-futures",
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
        bnc.range_to_lrvx(
            "BTCUSDT", "um-futures", "2024-01-15", "2024-01-16", self.tape,
            mirror=self.mirror, parallel=1, symbol_id=7,
        )
        stats = bnc.range_to_lrvx(
            "BTCUSDT", "um-futures", "2024-01-16", "2024-01-17", self.tape,
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
        ts = [r[5] * 1_000_000 for r in self.days[1]]
        for kwargs in calls:
            self.assertEqual(kwargs.get("from_ns"), min(ts))
            self.assertEqual(kwargs.get("to_ns"), max(ts))
            self.assertEqual(kwargs.get("symbols"), [7])


if __name__ == "__main__":
    unittest.main()
