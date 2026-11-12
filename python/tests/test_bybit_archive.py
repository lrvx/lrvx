"""Tests for ``lrvx.archives.bybit`` — Bybit public archive
converter and the shared `ArchiveReader` protocol contract.

No network by default: gzipped CSV in the layouts Bybit publishes
(header and row shapes copied from real files) is built in memory and
fed through the converter. The resulting `.lrvx` is read back via
DataReader and asserted trade-by-trade.

Covers:

  * `trades_to_lrvx` writes the exact row count + side mapping
    (Buy → BUY, Sell → SELL), for the derivatives layout with and
    without the RPI column and for the spot layout (ms timestamps,
    integer ids).
  * URLs: `trading/<SYM>/<SYM><DATE>` and `spot/<SYM>/<SYM>_<DATE>`.
  * A header Bybit does not publish raises UnknownLayoutError; a file
    whose rows all fail to parse raises NothingParsedError.
  * trade_id derivation from `trdMatchID` (UUID strings → folded
    int64 hash).
  * Dedup on re-run.
  * Multi-day append with UUID ids: a later day lands in full, a
    repeated day adds nothing, an overlapping range keeps each trade
    once.
  * `metadata.json` carries `exchange=bybit`, `instrument_type`.
  * `bybit` module satisfies the `ArchiveReader` Protocol.
  * Multi-symbol co-existence with a Binance tape merges cleanly via
    MergedTapeReader (cross-exchange same-base / same-quote works
    because each tape keys its own (exchange, name) namespace).
  * With LRVX_ARCHIVE_NETWORK_TESTS=1, real days downloaded from Bybit.

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_bybit_archive.py
"""
from __future__ import annotations

import gzip
import io
import json
import sys
import tempfile
import unittest
import uuid
from datetime import date
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
for cand in ("build/python", "build-py312/python"):
    p = REPO_ROOT / cand
    if p.is_dir():
        sys.path.insert(0, str(p))
        break
sys.path.insert(0, str(Path(__file__).resolve().parent))

import lrvx  # noqa: E402

from support import archive_network as net  # noqa: E402
from lrvx.archives import ArchiveReader, bybit  # noqa: E402


_HEADER = ("timestamp,symbol,side,size,price,tickDirection,trdMatchID,"
           "grossValue,homeNotional,foreignNotional")
_SPOT_HEADER = "id,timestamp,price,volume,side"

# Derivatives rows, shaped like BTCUSDT linear rows.
_ROWS = [
    ("1700000000.5000", "BTCUSDT", "Buy",  0.01, 42_100.5,
     "ZeroPlusTick",  "08ff9568-cb50-55d6-b497-13727eec09dc",
     "4.2100500000000005e+10", 0.01, 421.005),
    ("1700000001.2500", "BTCUSDT", "Sell", 0.02, 42_101.0,
     "ZeroMinusTick", "3f1d562a-57d4-5755-b305-2ade260deeaf",
     "8.4202e+10", 0.02, 842.02),
    ("1700000002.0000", "BTCUSDT", "Buy",  0.03, 42_100.7,
     "PlusTick",      "f0039dc6-a46b-5c4d-b550-cad3be2fca6e",
     "1.263021e+11", 0.03, 1263.021),
]

# Spot rows (id, timestamp_ms, price, volume, side), shaped like
# MNTUSDT spot rows.
_SPOT_ROWS = [
    (1, 1_794_268_824_419, 0.4504, 44.4, "buy"),
    (2, 1_794_268_832_748, 0.4504, 27.1, "buy"),
    (3, 1_794_268_840_002, 0.4503, 275.83, "sell"),
]


def _write_gz(dest: Path, text: str) -> Path:
    dest.parent.mkdir(parents=True, exist_ok=True)
    with gzip.open(dest, "wt", encoding="utf-8") as f:
        f.write(text)
    return dest


def _build_gz(dest: Path, *, rpi: bool = False) -> Path:
    buf = io.StringIO()
    buf.write(_HEADER + (",RPI" if rpi else "") + "\n")
    for r in _ROWS:
        buf.write(",".join(str(x) for x in r) + (",0" if rpi else "") + "\n")
    return _write_gz(dest, buf.getvalue())


def _build_spot_gz(dest: Path, *, rpi: bool = False) -> Path:
    buf = io.StringIO()
    buf.write(_SPOT_HEADER + (",rpi" if rpi else "") + "\n")
    for r in _SPOT_ROWS:
        buf.write(",".join(str(x) for x in r) + (",0" if rpi else "") + "\n")
    return _write_gz(dest, buf.getvalue())


class BybitConvertTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="bybit-"))
        self.gz_path = self.tmp / "BTCUSDT2024-01-15.csv.gz"
        self.tape = self.tmp / "tape"

    def _convert(self) -> bybit.ConvertStats:
        _build_gz(self.gz_path)
        return bybit.trades_to_lrvx(
            self.gz_path, self.tape,
            symbol_id=7, symbol_name="BTCUSDT", market="linear",
        )

    def test_round_trip_and_side_mapping(self) -> None:
        stats = self._convert()
        self.assertEqual(stats.trades_written, 3)
        trades = lrvx.DataReader(str(self.tape)).read_trades()
        self.assertEqual(int(trades.size), 3)
        # Buy → 0, Sell → 1 by exchange order in the fixture.
        sides = [int(t["side"]) for t in trades]
        self.assertEqual(sides, [0, 1, 0])
        # Timestamps: 1_700_000_000.500 → 1_700_000_000_500_000_000 ns.
        ts_ns = [int(t["exchange_ts_ns"]) for t in trades]
        self.assertEqual(ts_ns,
                         [1_700_000_000_500_000_000,
                          1_700_000_001_250_000_000,
                          1_700_000_002_000_000_000])

    def test_dedup_on_rerun(self) -> None:
        first = self._convert()
        self.assertEqual(first.trades_written, 3)
        # Re-run: every trade_id is already on the tape → all skipped.
        second = bybit.trades_to_lrvx(
            self.gz_path, self.tape,
            symbol_id=7, symbol_name="BTCUSDT", market="linear",
        )
        self.assertEqual(second.trades_written, 0)
        self.assertEqual(second.rows_skipped, 3)
        trades = lrvx.DataReader(str(self.tape)).read_trades()
        self.assertEqual(int(trades.size), 3)

    def test_metadata(self) -> None:
        self._convert()
        meta = json.loads((self.tape / "metadata.json").read_text())
        self.assertEqual(meta["exchange"], "bybit")
        self.assertEqual(meta["instrument_type"], "perpetual")
        self.assertEqual(meta["total_trades"], 3)
        sym = meta["symbols"][0]
        self.assertEqual(sym["name"], "BTCUSDT")
        self.assertEqual(sym["base_asset"], "BTC")
        self.assertEqual(sym["quote_asset"], "USDT")

    def test_protocol_compliance(self) -> None:
        # The runtime_checkable Protocol matches at module level.
        self.assertIsInstance(bybit, ArchiveReader)

    def test_rpi_column_layout(self) -> None:
        _build_gz(self.gz_path, rpi=True)
        stats = bybit.trades_to_lrvx(self.gz_path, self.tape, market="linear")
        self.assertEqual(stats.trades_written, 3)
        trades = lrvx.DataReader(str(self.tape)).read_trades()
        self.assertEqual([int(t["qty_raw"]) for t in trades],
                         [1_000_000, 2_000_000, 3_000_000])

    def test_spot_layout(self) -> None:
        for rpi in (False, True):
            gz = _build_spot_gz(self.tmp / f"rpi{rpi}" / "MNTUSDT_2026-11-10.csv.gz",
                                rpi=rpi)
            tape = self.tmp / f"spot-{rpi}"
            stats = bybit.trades_to_lrvx(gz, tape, market="spot")
            self.assertEqual(stats.trades_written, 3)
            t = lrvx.DataReader(str(tape)).read_trades()
            got = [(int(t["trade_id"][i]), int(t["exchange_ts_ns"][i]),
                    int(t["price_raw"][i]), int(t["qty_raw"][i]), int(t["side"][i]))
                   for i in range(t.size)]
            self.assertEqual(got, [(i, ts * 1_000_000, round(p * 1e8),
                                    round(q * 1e8), 0 if s == "buy" else 1)
                                   for i, ts, p, q, s in _SPOT_ROWS])
            meta = json.loads((tape / "metadata.json").read_text())
            self.assertEqual(meta["symbols"][0]["name"], "MNTUSDT")
            self.assertEqual(meta["instrument_type"], "spot")
            self.assertEqual(meta["custom"], {"qty_unit": "base"})

    def test_spot_file_is_not_a_derivatives_file(self) -> None:
        gz = _build_spot_gz(self.tmp / "MNTUSDT_2026-11-10.csv.gz")
        with self.assertRaises(bybit.UnknownLayoutError):
            bybit.trades_to_lrvx(gz, self.tape, market="linear")
        with self.assertRaises(bybit.UnknownLayoutError):
            bybit.trades_to_lrvx(_build_gz(self.gz_path), self.tape,
                                 market="spot")
        self.assertFalse(self.tape.exists())

    def test_unknown_header_is_rejected(self) -> None:
        gz = _write_gz(self.gz_path, "time,side,qty,px\n1700000000.5,Buy,1,2\n")
        with self.assertRaises(bybit.UnknownLayoutError) as ctx:
            bybit.trades_to_lrvx(gz, self.tape, market="linear")
        self.assertIn("trdmatchid", str(ctx.exception))
        self.assertFalse(self.tape.exists())

    def test_nothing_parsed_is_an_error(self) -> None:
        gz = _write_gz(self.gz_path, _HEADER + "\n"
                       "yesterday,BTCUSDT,Buy,0.01,42100.5,PlusTick,a-b,1,1,1\n"
                       "1700000000.5,BTCUSDT,Hold,0.01,42100.5,PlusTick,c-d,1,1,1\n")
        with self.assertRaises(bybit.NothingParsedError):
            bybit.trades_to_lrvx(gz, self.tape, market="linear")
        self.assertFalse(self.tape.exists())

    def test_inverse_quantity_unit(self) -> None:
        _build_gz(self.gz_path)
        bybit.trades_to_lrvx(self.gz_path, self.tape, market="inverse",
                             symbol_name="BTCUSD")
        meta = json.loads((self.tape / "metadata.json").read_text())
        self.assertEqual(meta["custom"], {"qty_unit": "usd_contracts"})

    def test_archive_urls(self) -> None:
        day = date(2024, 1, 15)
        self.assertEqual(bybit._archive_url("BTCUSDT", "linear", day),
                         "https://public.bybit.com/trading/BTCUSDT/BTCUSDT2024-01-15.csv.gz")
        self.assertEqual(bybit._archive_url("BTCUSD", "inverse", day),
                         "https://public.bybit.com/trading/BTCUSD/BTCUSD2024-01-15.csv.gz")
        self.assertEqual(bybit._archive_url("BTCUSDT", "spot", day),
                         "https://public.bybit.com/spot/BTCUSDT/BTCUSDT_2024-01-15.csv.gz")


_DAY1_START_S = 1_705_276_800  # 2024-01-15T00:00:00Z
_DAY_S = 86_400
_ROWS_PER_DAY = 40


def _day_rows(day_index: int) -> list:
    """Bybit rows for one synthetic day with UUID-style trdMatchIDs.
    Their folded ids are unordered, so every later day carries ids
    below the maximum of the days before it."""
    start = _DAY1_START_S + day_index * _DAY_S
    rows = []
    for i in range(_ROWS_PER_DAY):
        trd = str(uuid.uuid5(uuid.NAMESPACE_OID, f"bybit-{day_index}-{i}"))
        qty, price = 0.01 * (i + 1), 42_000.0 + i
        rows.append((f"{start + i * 60}.125", "BTCUSDT",
                     "Buy" if i % 2 == 0 else "Sell", qty, price,
                     "PlusTick", trd, f"{qty * price * 1e8!r}", qty,
                     qty * price))
    return rows


def _write_day_gz(dest: Path, rows: list) -> Path:
    buf = io.StringIO()
    buf.write(_HEADER + "\n")
    for r in rows:
        buf.write(",".join(str(x) for x in r) + "\n")
    return _write_gz(dest, buf.getvalue())


class BybitMultiDayAppendTests(unittest.TestCase):
    """Append dedup must key on trade-id membership: folded UUID ids
    carry no order, so a later day is not "older" than the tape."""

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="bybit-days-"))
        self.mirror = self.tmp / "mirror"
        self.tape = self.tmp / "tape"
        self.day1 = _day_rows(0)
        self.day2 = _day_rows(1)
        self.day1_gz = _write_day_gz(
            bybit._mirror_path(self.mirror, "BTCUSDT", "linear",
                               date(2024, 1, 15)), self.day1)
        self.day2_gz = _write_day_gz(
            bybit._mirror_path(self.mirror, "BTCUSDT", "linear",
                               date(2024, 1, 16)), self.day2)

    def _import(self, path: Path) -> bybit.ConvertStats:
        return bybit.trades_to_lrvx(
            path, self.tape, symbol_id=7, symbol_name="BTCUSDT",
            market="linear",
        )

    def _tape_ids(self) -> list:
        trades = lrvx.DataReader(str(self.tape)).read_trades()
        return [int(t["trade_id"]) for t in trades]

    def _expected_ids(self, rows: list) -> list:
        return [bybit._hash_trade_id(r[6]) for r in rows]

    def test_fixture_ids_are_unordered_across_days(self) -> None:
        day1_max = max(self._expected_ids(self.day1))
        below = [i for i in self._expected_ids(self.day2) if i <= day1_max]
        self.assertGreater(len(below), 0)

    def test_two_days_in_sequence_both_land_in_full(self) -> None:
        first = self._import(self.day1_gz)
        second = self._import(self.day2_gz)
        self.assertEqual(first.trades_written, _ROWS_PER_DAY)
        self.assertEqual(second.trades_written, _ROWS_PER_DAY)
        self.assertEqual(second.rows_skipped, 0)
        self.assertEqual(sorted(self._tape_ids()),
                         sorted(self._expected_ids(self.day1 + self.day2)))

    def test_reimport_of_same_day_adds_nothing(self) -> None:
        self._import(self.day1_gz)
        self._import(self.day2_gz)
        again = self._import(self.day2_gz)
        self.assertEqual(again.trades_written, 0)
        self.assertEqual(again.rows_skipped, _ROWS_PER_DAY)
        again = self._import(self.day1_gz)
        self.assertEqual(again.trades_written, 0)
        self.assertEqual(again.rows_skipped, _ROWS_PER_DAY)
        self.assertEqual(len(self._tape_ids()), 2 * _ROWS_PER_DAY)

    def test_overlapping_range_keeps_each_trade_once(self) -> None:
        bybit.range_to_lrvx(
            "BTCUSDT", "linear", "2024-01-15", "2024-01-15", self.tape,
            mirror=self.mirror, parallel=1, symbol_id=7,
        )
        stats = bybit.range_to_lrvx(
            "BTCUSDT", "linear", "2024-01-15", "2024-01-16", self.tape,
            mirror=self.mirror, parallel=1, symbol_id=7,
        )
        self.assertEqual(stats.trades_written, _ROWS_PER_DAY)
        self.assertEqual(stats.rows_skipped, _ROWS_PER_DAY)
        ids = self._tape_ids()
        self.assertEqual(len(ids), len(set(ids)))
        self.assertEqual(sorted(ids),
                         sorted(self._expected_ids(self.day1 + self.day2)))
        meta = json.loads((self.tape / "metadata.json").read_text())
        self.assertEqual(meta["total_trades"], 2 * _ROWS_PER_DAY)


class CrossExchangeMergeTests(unittest.TestCase):
    """Tape from Bybit + tape from Binance under the same name key
    cleanly via MergedTapeReader (each side keeps its own global
    symbol id and exchange tag)."""

    def setUp(self) -> None:
        from lrvx.archives import binance as bnc
        import zipfile

        self.tmp = Path(tempfile.mkdtemp(prefix="bybit-merge-"))
        # Binance fixture
        bnz = self.tmp / "BTCUSDT-aggTrades-2024-01-15.zip"
        buf = io.StringIO()
        buf.write("agg_trade_id,price,quantity,first_trade_id,last_trade_id,"
                  "transact_time,is_buyer_maker\n")
        for i in range(2):
            buf.write(
                f"{1000+i},42000.0,0.01,{i},{i},"
                f"{1_700_000_000_500 + i * 1000},true\n"
            )
        with zipfile.ZipFile(bnz, "w") as z:
            z.writestr(bnz.with_suffix(".csv").name, buf.getvalue())
        self.bnz_tape = self.tmp / "binance-tape"
        bnc.aggtrades_to_lrvx(
            bnz, self.bnz_tape, symbol_id=1, symbol_name="BTCUSDT",
            market="um-futures",
        )
        # Bybit fixture
        gz = self.tmp / "BTCUSDT2024-01-15.csv.gz"
        _build_gz(gz)
        self.byb_tape = self.tmp / "bybit-tape"
        bybit.trades_to_lrvx(
            gz, self.byb_tape, symbol_id=1, symbol_name="BTCUSDT",
            market="linear",
        )

    def test_merge_two_exchanges(self) -> None:
        reader = lrvx.MergedTapeReader([
            str(self.bnz_tape), str(self.byb_tape),
        ])
        trades = reader.read_trades()
        # 2 binance + 3 bybit = 5 merged trade events.
        self.assertEqual(int(trades.size), 5)
        # Symbol table assigns one global id per (exchange, name).
        sym_table = reader.symbol_table()
        names = sorted({s["name"] for s in sym_table})
        self.assertEqual(names, ["BTCUSDT"])
        exchanges = sorted({s["exchange"] for s in sym_table})
        self.assertEqual(exchanges, ["binance", "bybit"])


def _decode_derivatives(row):
    secs, frac = row[0].split(".")
    return (None, float(row[4]), float(row[3]),
            int(secs) * 1_000_000_000 + int(frac.ljust(9, "0")),
            0 if row[2] == "Buy" else 1)


def _decode_spot(row):
    return (int(row[0]), float(row[2]), float(row[3]), int(row[1]) * 1_000_000,
            0 if row[4] == "buy" else 1)


@unittest.skipUnless(net.ENABLED, net.SKIP_REASON)
class BybitRealArchiveTests(unittest.TestCase):
    """One real day per market, downloaded from public.bybit.com."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="bybit-net-")
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)

    def _check(self, symbol: str, market: str, day: str, decode) -> None:
        tape = self.tmp / f"{market}-tape"
        mirror = self.tmp / "mirror"
        stats = bybit.range_to_lrvx(symbol, market, day, day, tape,
                                    mirror=mirror, parallel=1)
        self.assertGreater(stats.trades_written, 0)
        self.assertEqual(stats.rows_invalid, 0)
        path = bybit._mirror_path(mirror, symbol, market, date.fromisoformat(day))
        net.assert_sample(self, tape, net.expected_from(path, decode))

    def test_linear(self) -> None:
        self._check("MNTUSDT", "linear", "2026-11-10", _decode_derivatives)

    def test_inverse(self) -> None:
        self._check("ADAUSD", "inverse", "2026-11-10", _decode_derivatives)

    def test_spot(self) -> None:
        self._check("MNTUSDT", "spot", "2026-11-10", _decode_spot)


if __name__ == "__main__":
    unittest.main()
