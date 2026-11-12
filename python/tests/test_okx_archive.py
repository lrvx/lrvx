"""Tests for ``lrvx.archives.okx`` — OKX public archive converter.

No network by default: CSV in the layouts OKX publishes (header and
row shapes copied from real files) is built in memory and fed through
the converter. The resulting `.lrvx` is read back via DataReader and
asserted trade-by-trade.

Covers:

  * `trades_to_lrvx` round-trip + side mapping (buy → 0, sell → 1, any
    case) on the current layout, the layout with a `source` column and
    the legacy per-instrument layout whose header carries GBK-encoded
    Chinese names.
  * `.zip` and `.csv.gz` packaging both parse.
  * Futures / option chain files: rows of the requested instrument only;
    a chain file without a symbol, or without a trade of it, is an error.
  * A header OKX does not publish raises UnknownLayoutError and writes
    nothing; a file whose rows all fail to parse raises
    NothingParsedError; a file whose rows are all on the tape already is
    a legitimate empty import.
  * URLs per market, `qty_multiplier`, `metadata.json`.
  * Dedup on re-run and across days in any order.
  * Protocol compliance and a cross-exchange merge.
  * With LRVX_ARCHIVE_NETWORK_TESTS=1, real days downloaded from OKX.

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_okx_archive.py
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
from lrvx.archives import ArchiveReader, okx  # noqa: E402

_HEADER = "instrument_name,trade_id,side,price,size,created_time"
_HEADER_SOURCE = _HEADER + ",source"
# The legacy header as OKX writes it: each English name followed by "/"
# and its Chinese name, in GBK.
_HEADER_LEGACY = ("trade_id/交易id,side/交易方向,size/数量,price/价格,"
                  "created_time/成交时间")

# (trade_id, side, size, price, created_time), shaped like BTC-USDT-SWAP
# rows of 2024-01-15.
_ROWS = [
    (720073478, "buy",  8.0, 42897.9, 1_705_248_000_179),
    (720073479, "sell", 1.0, 42897.8, 1_705_248_000_412),
    (720073480, "buy", 25.0, 42898.0, 1_705_248_001_003),
]


def _csv_bytes(rows=_ROWS, *, inst="BTC-USDT-SWAP", layout="current") -> bytes:
    lines = []
    if layout == "current":
        lines.append(_HEADER)
        lines += [f"{inst},{t},{s},{p},{q},{ts}" for t, s, q, p, ts in rows]
    elif layout == "source":
        lines.append(_HEADER_SOURCE)
        lines += [f"{inst},{t},{s},{p},{q},{ts},0" for t, s, q, p, ts in rows]
    elif layout == "legacy":
        lines.append(_HEADER_LEGACY)
        lines += [f"{t},{s},{q},{p},{ts}" for t, s, q, p, ts in rows]
    else:
        raise ValueError(layout)
    return ("\n".join(lines) + "\n").encode("gbk")


def _build_zip(dest: Path, rows=_ROWS, **kw) -> Path:
    dest.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(dest.with_suffix(".csv").name, _csv_bytes(rows, **kw))
    return dest


def _build_raw_zip(dest: Path, text: str) -> Path:
    with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(dest.with_suffix(".csv").name, text)
    return dest


def _build_gz(dest: Path) -> Path:
    with gzip.open(dest, "wb") as f:
        f.write(_csv_bytes())
    return dest


def _trades(tape: Path) -> list:
    t = lrvx.DataReader(str(tape)).read_trades()
    return [(int(t["trade_id"][i]), int(t["side"][i]), int(t["qty_raw"][i]),
             int(t["price_raw"][i]), int(t["exchange_ts_ns"][i]))
            for i in range(t.size)]


def _expected(rows=_ROWS, mult=1.0) -> list:
    return [(t, 0 if s.lower() == "buy" else 1, round(q * mult * 1e8),
             round(p * 1e8), ts * 1_000_000) for t, s, q, p, ts in rows]


class OkxConvertTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="okx-"))
        self.tape = self.tmp / "tape"

    def _convert_zip(self, **kw) -> okx.ConvertStats:
        z = self.tmp / "BTC-USDT-SWAP-trades-2024-01-15.zip"
        _build_zip(z, **{k: v for k, v in kw.items() if k == "layout"})
        return okx.trades_to_lrvx(
            z, self.tape, symbol_id=7, market="swap",
            **{k: v for k, v in kw.items() if k != "layout"})

    def test_round_trip_zip(self) -> None:
        stats = self._convert_zip()
        self.assertEqual(stats.trades_written, 3)
        self.assertEqual(stats.rows_invalid, 0)
        self.assertEqual(_trades(self.tape), _expected())

    def test_source_column_layout(self) -> None:
        stats = self._convert_zip(layout="source")
        self.assertEqual(stats.trades_written, 3)
        self.assertEqual(_trades(self.tape), _expected())

    def test_legacy_layout_with_gbk_header(self) -> None:
        stats = self._convert_zip(layout="legacy")
        self.assertEqual(stats.trades_written, 3)
        self.assertEqual(_trades(self.tape), _expected())

    def test_upper_case_side(self) -> None:
        rows = [(t, s.upper(), q, p, ts) for t, s, q, p, ts in _ROWS]
        z = _build_zip(self.tmp / "BTC-USDT-SWAP-trades-2022-01-01.zip", rows)
        okx.trades_to_lrvx(z, self.tape, market="swap")
        self.assertEqual(_trades(self.tape), _expected(rows))

    def test_round_trip_gz(self) -> None:
        g = self.tmp / "BTC-USDT-SWAP-trades-2024-01-15.csv.gz"
        _build_gz(g)
        stats = okx.trades_to_lrvx(
            g, self.tape,
            symbol_id=7, symbol_name="BTC-USDT-SWAP", market="swap",
        )
        self.assertEqual(stats.trades_written, 3)

    def test_dedup_on_rerun(self) -> None:
        first = self._convert_zip()
        self.assertEqual(first.trades_written, 3)
        second = self._convert_zip()
        self.assertEqual(second.trades_written, 0)
        self.assertEqual(second.rows_skipped, 3)
        self.assertEqual(second.rows_read, 3)

    def test_unknown_header_is_rejected(self) -> None:
        z = _build_raw_zip(self.tmp / "BTC-USDT-SWAP-trades-2024-01-15.zip",
                           "trade_id,side,size,price,timestamp_ms\n"
                           "100,buy,0.01,42100.5,1700000000500\n")
        with self.assertRaises(okx.UnknownLayoutError) as ctx:
            okx.trades_to_lrvx(z, self.tape, market="swap")
        self.assertIn("timestamp_ms", str(ctx.exception))
        self.assertIn("instrument_name,trade_id,side,price,size,created_time",
                      str(ctx.exception))
        self.assertFalse(self.tape.exists())

    def test_nothing_parsed_is_an_error(self) -> None:
        z = _build_raw_zip(self.tmp / "BTC-USDT-SWAP-trades-2024-01-15.zip",
                           _HEADER + "\n"
                           "BTC-USDT-SWAP,1,buy,n/a,8.0,1705248000179\n"
                           "BTC-USDT-SWAP,2,hold,42897.9,8.0,1705248000180\n")
        with self.assertRaises(okx.NothingParsedError) as ctx:
            okx.trades_to_lrvx(z, self.tape, market="swap")
        self.assertIn("2 of 2", str(ctx.exception))
        self.assertFalse(self.tape.exists())

    def test_some_rows_invalid_are_counted(self) -> None:
        z = _build_raw_zip(self.tmp / "BTC-USDT-SWAP-trades-2024-01-15.zip",
                           _HEADER + "\n"
                           "BTC-USDT-SWAP,1,buy,42897.9,8.0,1705248000179\n"
                           "BTC-USDT-SWAP,2,buy,,8.0,1705248000180\n")
        stats = okx.trades_to_lrvx(z, self.tape, market="swap")
        self.assertEqual((stats.trades_written, stats.rows_invalid), (1, 1))

    def test_header_only_file_is_empty_not_an_error(self) -> None:
        z = _build_raw_zip(self.tmp / "BTC-USDT-SWAP-trades-2024-01-15.zip",
                           _HEADER + "\n")
        stats = okx.trades_to_lrvx(z, self.tape, market="swap")
        self.assertEqual((stats.trades_written, stats.rows_read), (0, 0))

    def test_qty_multiplier_and_metadata(self) -> None:
        self._convert_zip(qty_multiplier=0.01)
        self.assertEqual(_trades(self.tape), _expected(mult=0.01))
        meta = json.loads((self.tape / "metadata.json").read_text())
        self.assertEqual(meta["custom"]["qty_unit"], "contracts")
        self.assertEqual(meta["custom"]["qty_multiplier"], 0.01)

    def test_metadata(self) -> None:
        self._convert_zip()
        meta = json.loads((self.tape / "metadata.json").read_text())
        self.assertEqual(meta["exchange"], "okx")
        self.assertEqual(meta["instrument_type"], "perpetual")
        self.assertEqual(meta["total_trades"], 3)
        self.assertEqual(meta["custom"], {"qty_unit": "contracts",
                                          "qty_multiplier": 1.0})
        sym = meta["symbols"][0]
        self.assertEqual(sym["name"], "BTC-USDT-SWAP")
        self.assertEqual(sym["base_asset"], "BTC")
        self.assertEqual(sym["quote_asset"], "USDT")

    def test_spot_quantity_unit(self) -> None:
        z = _build_zip(self.tmp / "BTC-USDT-trades-2024-01-15.zip",
                       inst="BTC-USDT")
        okx.trades_to_lrvx(z, self.tape, market="spot")
        meta = json.loads((self.tape / "metadata.json").read_text())
        self.assertEqual(meta["custom"]["qty_unit"], "base")
        self.assertEqual(meta["symbols"][0]["name"], "BTC-USDT")

    def test_protocol_compliance(self) -> None:
        self.assertIsInstance(okx, ArchiveReader)

    def test_archive_urls(self) -> None:
        day = date(2024, 1, 15)
        base = "https://static.okx.com/cdn/okex/traderecords/trades/daily/20240115/"
        self.assertEqual(okx._archive_url("BTC-USDT", "spot", day),
                         base + "BTC-USDT-trades-2024-01-15.zip")
        self.assertEqual(okx._archive_url("BTC-USDT-SWAP", "swap", day),
                         base + "BTC-USDT-SWAP-trades-2024-01-15.zip")
        self.assertEqual(okx._archive_url("BTC-USD-240329", "futures", day),
                         base + "BTC-USD-futureschain-trades-2024-01-15.zip")
        self.assertEqual(okx._archive_url("BTC-USD-240329-50000-C", "option", day),
                         base + "BTC-USD-optionchain-trades-2024-01-15.zip")

    def test_symbol_must_match_market(self) -> None:
        for symbol, market in (("BTC-USDT", "swap"), ("BTC-USDT-SWAP", "futures"),
                               ("BTC-29MAR24-50000-C", "option"),
                               ("BTC-USD-240329", "deliverable")):
            with self.assertRaises(ValueError):
                okx._archive_url(symbol, market, date(2024, 1, 15))


class OkxChainFileTests(unittest.TestCase):
    """Futures and options come as one chain file per family and day."""

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="okx-chain-"))
        self.tape = self.tmp / "tape"
        self.path = self.tmp / "BTC-USD-futureschain-trades-2024-01-15.zip"
        march = [(113830, "buy", 1.0, 42946.3, 1_705_248_020_502),
                 (113831, "buy", 16.0, 42946.3, 1_705_248_020_502)]
        june = [(90001, "sell", 0.2, 43500.1, 1_705_248_030_000)]
        text = (_HEADER + "\n"
                + "".join(f"BTC-USD-240329,{t},{s},{p},{q},{ts}\n"
                          for t, s, q, p, ts in march)
                + "".join(f"BTC-USD-240628,{t},{s},{p},{q},{ts}\n"
                          for t, s, q, p, ts in june))
        _build_raw_zip(self.path, text)
        self.march, self.june = march, june

    def test_keeps_only_the_requested_instrument(self) -> None:
        stats = okx.trades_to_lrvx(self.path, self.tape, market="futures",
                                   symbol_name="BTC-USD-240628")
        self.assertEqual(stats.trades_written, 1)
        self.assertEqual(stats.rows_other_instruments, 2)
        self.assertEqual(_trades(self.tape), _expected(self.june))

    def test_chain_file_needs_a_symbol(self) -> None:
        with self.assertRaises(ValueError) as ctx:
            okx.trades_to_lrvx(self.path, self.tape, market="futures")
        self.assertIn("symbol_name", str(ctx.exception))

    def test_instrument_absent_from_the_file(self) -> None:
        with self.assertRaises(okx.InstrumentNotInFileError) as ctx:
            okx.trades_to_lrvx(self.path, self.tape, market="futures",
                               symbol_name="BTC-USD-240927")
        self.assertIn("BTC-USD-240329", str(ctx.exception))
        self.assertFalse(self.tape.exists())

    def test_range_skips_days_without_the_instrument(self) -> None:
        mirror = self.tmp / "mirror"
        day1 = okx._mirror_path(mirror, "BTC-USD-240628", "futures",
                                date(2024, 1, 15))
        day1.parent.mkdir(parents=True)
        day1.write_bytes(self.path.read_bytes())
        day2 = okx._mirror_path(mirror, "BTC-USD-240628", "futures",
                                date(2024, 1, 16))
        _build_zip(day2, self.march, inst="BTC-USD-240329")
        stats = okx.range_to_lrvx("BTC-USD-240628", "futures", "2024-01-15",
                                  "2024-01-16", self.tape, mirror=mirror,
                                  parallel=1)
        self.assertEqual((stats.trades_written, stats.files_processed), (1, 2))
        with self.assertRaises(okx.InstrumentNotInFileError):
            okx.range_to_lrvx("BTC-USD-240628", "futures", "2024-01-16",
                              "2024-01-16", self.tmp / "tape2", mirror=mirror,
                              parallel=1)

    def test_option_market_metadata(self) -> None:
        z = _build_zip(self.tmp / "BTC-USD-optionchain-trades-2024-01-15.zip",
                       [(5, "buy", 600.0, 0.0001, 1_705_262_579_715)],
                       inst="BTC-USD-240115-38000-P")
        okx.trades_to_lrvx(z, self.tape, symbol_id=7, market="option",
                           symbol_name="BTC-USD-240115-38000-P")
        meta = json.loads((self.tape / "metadata.json").read_text())
        self.assertEqual(meta["instrument_type"], "option")
        sym = meta["symbols"][0]
        self.assertEqual((sym["base_asset"], sym["quote_asset"]), ("BTC", "USD"))


class CrossExchangeMergeTests(unittest.TestCase):
    def setUp(self) -> None:
        from lrvx.archives import binance as bnc

        self.tmp = Path(tempfile.mkdtemp(prefix="okx-xmrg-"))
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
        z = self.tmp / "BTC-USDT-SWAP-trades-2024-01-15.zip"
        _build_zip(z)
        self.okx_tape = self.tmp / "okx-tape"
        okx.trades_to_lrvx(
            z, self.okx_tape, symbol_id=1, symbol_name="BTC-USDT-SWAP",
            market="swap",
        )

    def test_merge(self) -> None:
        reader = lrvx.MergedTapeReader([
            str(self.bnz_tape), str(self.okx_tape),
        ])
        trades = reader.read_trades()
        self.assertEqual(int(trades.size), 5)
        sym_table = reader.symbol_table()
        exchanges = sorted({s["exchange"] for s in sym_table})
        self.assertEqual(exchanges, ["binance", "okx"])
        names = sorted({s["name"] for s in sym_table})
        # Two distinct names; the two tapes do not collide.
        self.assertEqual(names, ["BTC-USDT-SWAP", "BTCUSDT"])


_DAY1_START_MS = 1_705_276_800_000  # 2024-01-15T00:00:00Z
_DAY_MS = 86_400_000
_ROWS_PER_DAY = 40


def _day_rows(day_index: int) -> list:
    """OKX rows for one synthetic day. Ids increase with time
    across days, as on the exchange."""
    start = _DAY1_START_MS + day_index * _DAY_MS
    base = 50_000 + day_index * _ROWS_PER_DAY
    return [(base + i, "buy" if i % 2 == 0 else "sell", float(i + 1),
             42_000.0 + i, start + i * 60_000)
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


class OkxMultiDayAppendTests(unittest.TestCase):
    """Append dedup across days: every day lands once, whatever order
    the days are imported in, and the dedup read covers only the
    incoming day's time range."""

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="okx-days-"))
        self.mirror = self.tmp / "mirror"
        self.tape = self.tmp / "tape"
        self.days = [_day_rows(i) for i in range(3)]
        self.paths = []
        for i, rows in enumerate(self.days):
            dest = okx._mirror_path(self.mirror, "BTC-USDT-SWAP", "swap",
                                    date(2024, 1, 15 + i))
            self.paths.append(_build_zip(dest, rows))

    def _import(self, day_index: int) -> okx.ConvertStats:
        return okx.trades_to_lrvx(
            self.paths[day_index], self.tape, symbol_id=7,
            symbol_name="BTC-USDT-SWAP", market="swap",
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
        okx.range_to_lrvx(
            "BTC-USDT-SWAP", "swap", "2024-01-15", "2024-01-16", self.tape,
            mirror=self.mirror, parallel=1, symbol_id=7,
        )
        stats = okx.range_to_lrvx(
            "BTC-USDT-SWAP", "swap", "2024-01-16", "2024-01-17", self.tape,
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


def _decode_okx(symbol: str):
    def decode(row):
        if row[0] != symbol:
            return None
        return (int(row[1]), float(row[3]), float(row[4]),
                int(row[5]) * 1_000_000, 0 if row[2].lower() == "buy" else 1)
    return decode


@unittest.skipUnless(net.ENABLED, net.SKIP_REASON)
class OkxRealArchiveTests(unittest.TestCase):
    """One real day per market, downloaded from OKX's CDN."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="okx-net-")
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)

    def _check(self, symbol: str, market: str, day: str) -> None:
        tape = self.tmp / f"{market}-tape"
        stats = okx.range_to_lrvx(symbol, market, day, day, tape,
                                  mirror=self.tmp / "mirror", parallel=1)
        self.assertGreater(stats.trades_written, 0)
        self.assertEqual(stats.rows_invalid, 0)
        path = okx._mirror_path(self.tmp / "mirror", symbol, market,
                                date.fromisoformat(day))
        net.assert_sample(self, tape, net.expected_from(path, _decode_okx(symbol)))

    def test_swap(self) -> None:
        self._check("BTC-USD-SWAP", "swap", "2024-01-15")

    def test_spot(self) -> None:
        self._check("BTC-USDT", "spot", "2024-01-15")

    def test_futures(self) -> None:
        self._check("BTC-USD-240329", "futures", "2024-01-15")

    def test_option(self) -> None:
        self._check("BTC-USD-240126-42000-P", "option", "2024-01-15")


if __name__ == "__main__":
    unittest.main()
