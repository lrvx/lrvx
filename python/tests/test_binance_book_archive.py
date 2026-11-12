"""Tests for ``lrvx.archives.binance.{t1_to_lrvx, range_book_to_lrvx}``.

No network by default: bookTicker CSVs in the layout published on
``data.binance.vision`` (header and row shapes copied from real files)
are built in-memory and fed through the converter. The resulting `.lrvx` directory is read back via
``lrvx.DataReader.read_book_updates`` and asserted ladder-by-ladder.

Covers:

  * `t1_to_lrvx`: initial snapshot + delta on the next change, no-op
    on unchanged ticks, dedup on re-run by `update_id`.
  * Unsupported requests: a book_type other than ``t1`` and the spot
    market fail with a clear error, in the library and on the CLI,
    before anything is downloaded or written.
  * An unknown header raises UnknownLayoutError; a file whose rows all
    fail to parse raises NothingParsedError.
  * Co-existence: aggTrades + book events sit in one tape, monotonic
    on `exchange_ts_ns` (the existing reader interleaves them).
  * `metadata.json`: `has_book_snapshots` / `has_book_deltas` /
    `total_book_updates` / `book_depth` populated by the writer.
  * Multi-day append: days in sequence, re-imports,
    overlapping ranges, out-of-order days, a gap fill and a day cut
    short each give the tape one in-order import of the same days
    gives, and the dedup read covers only the incoming day's time range.
  * With LRVX_ARCHIVE_NETWORK_TESTS=1, a real day downloaded from
    data.binance.vision.

Run from repo root:
    PYTHONPATH=build/python python3 python/tests/test_binance_book_archive.py
"""
from __future__ import annotations

import io
import json
import random
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

from support import archive_network as net  # noqa: E402
from lrvx import orderbook  # noqa: E402
from lrvx.archives import binance as bnc  # noqa: E402


# bookTicker rows: update_id, bid_p, bid_q, ask_p, ask_q,
# transaction_time, event_time. Three rows: one initial, one
# unchanged, one with new top.
_T1_ROWS = [
    (100, 42_000.0, 1.0, 42_001.0, 1.0, 1_700_000_000_000, 1_700_000_000_000),
    (101, 42_000.0, 1.0, 42_001.0, 1.0, 1_700_000_001_000, 1_700_000_001_000),
    (102, 41_999.0, 2.0, 42_002.0, 1.5, 1_700_000_002_000, 1_700_000_002_000),
]


_T1_HEADER = ("update_id,best_bid_price,best_bid_qty,best_ask_price,"
              "best_ask_qty,transaction_time,event_time")


def _build_t1_zip(dest: Path) -> Path:
    buf = io.StringIO()
    buf.write(_T1_HEADER + "\n")
    for r in _T1_ROWS:
        buf.write(",".join(str(x) for x in r) + "\n")
    with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(dest.with_suffix(".csv").name, buf.getvalue())
    return dest


def _read_events(tape_dir: Path):
    """Return list of dict(seq, ts_ns, is_snapshot, bids, asks)."""
    r = lrvx.DataReader(str(tape_dir))
    headers, levels = r.read_book_updates()
    events = []
    for h in headers:
        off = int(h["level_offset"])
        n_bid = int(h["bid_count"])
        n_ask = int(h["ask_count"])
        bids = [(int(levels[off + i]["price_raw"]),
                 int(levels[off + i]["qty_raw"]))
                for i in range(n_bid)]
        asks = [(int(levels[off + n_bid + i]["price_raw"]),
                 int(levels[off + n_bid + i]["qty_raw"]))
                for i in range(n_ask)]
        events.append(dict(
            seq=int(h["seq"]),
            ts_ns=int(h["exchange_ts_ns"]),
            symbol_id=int(h["symbol_id"]),
            # PyDataReader.readBookUpdates maps event_type from
            # book_header.type (0 = snapshot, 1 = delta).
            is_snapshot=int(h["event_type"]) == 0,
            bids=bids, asks=asks,
        ))
    return events


class T1Tests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="binance-t1-"))
        self.zip_path = self.tmp / "BTCUSDT-bookTicker-2024-01-15.zip"
        self.tape = self.tmp / "tape"

    def test_initial_snapshot_then_delta(self) -> None:
        _build_t1_zip(self.zip_path)
        stats = bnc.t1_to_lrvx(
            self.zip_path, self.tape,
            symbol_id=7, symbol_name="BTCUSDT", market="um-futures",
        )
        self.assertEqual(stats.snapshots_written, 1)
        self.assertEqual(stats.deltas_written, 1)
        self.assertEqual(stats.rows_skipped, 1)  # the unchanged tick

        events = _read_events(self.tape)
        self.assertEqual(len(events), 2)
        self.assertTrue(events[0]["is_snapshot"])
        self.assertFalse(events[1]["is_snapshot"])
        # Snapshot top-of-book matches the first row exactly.
        self.assertEqual(events[0]["bids"], [(4_200_000_000_000, 100_000_000)])
        self.assertEqual(events[0]["asks"], [(4_200_100_000_000, 100_000_000)])
        # Delta includes both removals (qty=0) and the new top.
        # Order is implementation-defined; check as sets.
        self.assertEqual(set(events[1]["bids"]),
                         {(4_199_900_000_000, 200_000_000),
                          (4_200_000_000_000, 0)})
        self.assertEqual(set(events[1]["asks"]),
                         {(4_200_200_000_000, 150_000_000),
                          (4_200_100_000_000, 0)})

    def test_dedup_on_rerun(self) -> None:
        _build_t1_zip(self.zip_path)
        bnc.t1_to_lrvx(self.zip_path, self.tape,
                          symbol_id=7, symbol_name="BTCUSDT",
                          market="um-futures")
        second = bnc.t1_to_lrvx(
            self.zip_path, self.tape,
            symbol_id=7, symbol_name="BTCUSDT", market="um-futures",
        )
        # Every row's update_id <= last_seq → all skipped.
        self.assertEqual(second.snapshots_written, 0)
        self.assertEqual(second.deltas_written, 0)
        self.assertEqual(second.rows_skipped, 3)
        events = _read_events(self.tape)
        self.assertEqual(len(events), 2)  # unchanged from first run


class T1LayoutTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="binance-t1-layout-"))
        self.zip_path = self.tmp / "BTCUSDT-bookTicker-2024-01-15.zip"
        self.tape = self.tmp / "tape"

    def _zip(self, text: str) -> Path:
        with zipfile.ZipFile(self.zip_path, "w") as zf:
            zf.writestr("x.csv", text)
        return self.zip_path

    def test_unknown_header_is_rejected(self) -> None:
        z = self._zip("update_id,bid,ask,time\n1,1.0,2.0,1700000000000\n")
        for append in (True, False):
            with self.assertRaises(bnc.UnknownLayoutError) as ctx:
                bnc.t1_to_lrvx(z, self.tape, market="um-futures", append=append)
            self.assertIn(_T1_HEADER, str(ctx.exception))
        self.assertFalse(self.tape.exists())

    def test_nothing_parsed_is_an_error(self) -> None:
        z = self._zip(_T1_HEADER + "\n"
                      "1,n/a,1.0,2.0,1.0,1700000000000,1700000000000\n"
                      "2,1.0,1.0,2.0,1.0,soon,1700000000001\n")
        with self.assertRaises(bnc.NothingParsedError):
            bnc.t1_to_lrvx(z, self.tape, market="um-futures")
        self.assertFalse(self.tape.exists())

    def test_header_only_file_is_empty_not_an_error(self) -> None:
        z = self._zip(_T1_HEADER + "\n")
        stats = bnc.t1_to_lrvx(z, self.tape, market="um-futures")
        self.assertEqual((stats.rows_read, stats.snapshots_written), (0, 0))


class UnsupportedBookRequestTests(unittest.TestCase):
    """Only bookTicker on um-futures / cm-futures is importable; every
    other request fails up front with a message naming what is
    supported."""

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="binance-unsupported-"))
        self.tape = self.tmp / "tape"
        self.zip_path = _build_t1_zip(self.tmp / "BTCUSDT-bookTicker-2024-01-15.zip")

    def _range(self, market: str, book_type: str, **kwargs):
        with mock.patch.object(bnc, "_download") as download:
            with self.assertRaises(ValueError) as cm:
                bnc.range_book_to_lrvx(
                    "BTCUSDT", market, book_type, "2024-01-15", "2024-01-16",
                    self.tape, mirror=self.tmp / "mirror", **kwargs)
        download.assert_not_called()
        self.assertFalse(self.tape.exists())
        return str(cm.exception)

    def test_depth20_book_type_is_rejected(self) -> None:
        msg = self._range("um-futures", "depth20")
        self.assertIn("depth20", msg)
        self.assertIn("'t1'", msg)

    def test_depth20_is_rejected_even_with_skip_missing(self) -> None:
        self._range("um-futures", "depth20", skip_missing=True)

    def test_spot_range_is_rejected(self) -> None:
        msg = self._range("spot", "t1")
        self.assertIn("spot", msg)
        self.assertIn("um-futures", msg)
        self.assertIn("cm-futures", msg)

    def test_spot_range_is_rejected_even_with_skip_missing(self) -> None:
        self._range("spot", "t1", skip_missing=True)

    def test_spot_single_file_is_rejected(self) -> None:
        with self.assertRaises(ValueError) as cm:
            bnc.t1_to_lrvx(self.zip_path, self.tape, market="spot")
        self.assertIn("spot", str(cm.exception))
        self.assertFalse(self.tape.exists())

    def test_futures_markets_build_bookticker_urls(self) -> None:
        day = date(2024, 1, 15)
        self.assertEqual(
            bnc._book_archive_url("BTCUSDT", "um-futures", "t1", day),
            "https://data.binance.vision/data/futures/um/daily/bookTicker/"
            "BTCUSDT/BTCUSDT-bookTicker-2024-01-15.zip")
        self.assertEqual(
            bnc._book_archive_url("BTCUSD_PERP", "cm-futures", "t1", day),
            "https://data.binance.vision/data/futures/cm/daily/bookTicker/"
            "BTCUSD_PERP/BTCUSD_PERP-bookTicker-2024-01-15.zip")

    def _cli(self, *argv: str):
        from lrvx import cli

        err = io.StringIO()
        code = None
        with mock.patch.object(sys, "stderr", err), \
                mock.patch.object(bnc, "_download") as download:
            try:
                code = cli.main(["archive", "binance", *argv,
                                 "--out", str(self.tape)])
            except SystemExit as exc:
                code = exc.code
        download.assert_not_called()
        self.assertFalse(self.tape.exists())
        return code, err.getvalue()

    def test_cli_book_depth20_fails(self) -> None:
        code, err = self._cli("--book", "depth20", "--symbol", "BTCUSDT",
                              "--from", "2024-01-15", "--to", "2024-01-15")
        self.assertEqual(code, 2)
        self.assertIn("--book", err)
        self.assertIn("invalid choice: 'depth20'", err)
        self.assertIn("t1", err.split("invalid choice", 1)[1])

    def test_cli_book_spot_fails(self) -> None:
        for extra in (("--symbol", "BTCUSDT", "--from", "2024-01-15",
                       "--to", "2024-01-15"),
                      ("--csv", str(self.zip_path))):
            code, err = self._cli("--book", "t1", "--market", "spot", *extra)
            self.assertEqual(code, 2)
            self.assertIn("spot", err)
            self.assertIn("um-futures", err)


class CoexistenceTests(unittest.TestCase):
    """aggTrades + book events in one tape — both readable via the
    existing reader surfaces, monotonic on exchange_ts_ns when
    interleaved (DataReader sorts by ts_ns on read)."""

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="binance-coex-"))
        self.tape = self.tmp / "tape"

    def test_trades_and_books_share_tape(self) -> None:
        # aggTrades archive
        agg_zip = self.tmp / "BTCUSDT-aggTrades-2024-01-15.zip"
        buf = io.StringIO()
        # (agg_id, price, qty, first_id, last_id, ts_ms, maker)
        buf.write("agg_trade_id,price,quantity,first_trade_id,last_trade_id,"
                  "transact_time,is_buyer_maker\n")
        for i in range(3):
            buf.write(f"{1000+i},42000.0,0.01,{i},{i},"
                      f"{1_700_000_000_500 + i * 1000},true\n")
        with zipfile.ZipFile(agg_zip, "w") as z:
            z.writestr(agg_zip.with_suffix(".csv").name, buf.getvalue())

        # bookTicker archive
        bt_zip = self.tmp / "BTCUSDT-bookTicker-2024-01-15.zip"
        _build_t1_zip(bt_zip)

        bnc.aggtrades_to_lrvx(agg_zip, self.tape, symbol_id=1,
                                 symbol_name="BTCUSDT", market="um-futures")
        bnc.t1_to_lrvx(bt_zip, self.tape, symbol_id=1,
                          symbol_name="BTCUSDT", market="um-futures")

        # Trade rows readable.
        r = lrvx.DataReader(str(self.tape))
        trades = r.read_trades()
        self.assertEqual(int(trades.size), 3)
        # Book events readable.
        events = _read_events(self.tape)
        self.assertEqual(len(events), 2)
        # Metadata reflects both streams.
        meta = json.loads((self.tape / "metadata.json").read_text())
        self.assertTrue(meta["has_trades"])
        self.assertTrue(meta["has_book_snapshots"])
        self.assertGreaterEqual(meta["total_trades"], 3)
        self.assertGreaterEqual(meta["total_book_updates"], 2)


# ── Multi-day append ───────────────────────────────────────────────
#
# Synthetic days whose update ids run on across day boundaries and
# whose timestamps never meet across days, as on data.binance.vision.
# "The same tape" below means three things, each checked against one
# in-order import of the same days into an empty tape:
#
#   1. the same book records: equal (exchange_ts, seq, type, bids,
#      asks) multisets, so any replay that orders by timestamp sees the
#      same stream;
#   2. the same ladder from ``orderbook.book_at`` (the documented
#      timestamp-ordered reconstruction) at every source timestamp,
#      just before each day starts and after the last one;
#   3. the same ladder after every event when the tape is replayed in
#      its stored order (segments by file name, as ``DataReader``
#      streams them, with no re-sort), so a consumer that does not
#      re-sort across segments sees the right book after each event.

_DAY0_MS = 1_705_276_800_000  # 2024-01-15T00:00:00Z
_DAY_MS = 86_400_000
_N_DAYS = 3
_SYM = 7


def _t1_days() -> list:
    """bookTicker rows for _N_DAYS days. Some rows repeat the previous
    top, some share a timestamp with the row before."""
    rng = random.Random(11)
    days = []
    uid = 3_831_603_310_000
    bid = 42_000.0
    for d in range(_N_DAYS):
        rows = []
        ts = _DAY0_MS + d * _DAY_MS + 15
        for _ in range(60):
            uid += rng.randint(1, 5)
            ts += rng.choice((0, 0, 7, 1_000, 60_000))
            if rng.random() < 0.7:
                bid += rng.choice((-1.0, -0.5, 0.0, 0.5, 1.0))
            bid_q = rng.choice((0.5, 1.0, 1.5))
            ask_q = rng.choice((0.25, 2.0))
            rows.append((uid, bid, bid_q, bid + 0.5, ask_q, ts, ts + 4))
        days.append(rows)
    return days


def _write_zip(dest: Path, rows) -> Path:
    dest.parent.mkdir(parents=True, exist_ok=True)
    buf = io.StringIO()
    buf.write(_T1_HEADER + "\n")
    for r in rows:
        buf.write(",".join(str(x) for x in r) + "\n")
    with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(dest.with_suffix(".csv").name, buf.getvalue())
    return dest


def _record_reader_ranges():
    """Patch ``lrvx.DataReader`` to record the keyword arguments of each
    construction; returns the patcher and the list it fills."""
    calls: list = []
    real = lrvx.DataReader

    def _wrapped(*args, **kwargs):
        calls.append(kwargs)
        return real(*args, **kwargs)

    return mock.patch.object(lrvx, "DataReader", _wrapped), calls


def _records(tape: Path) -> list:
    """Book records in stored order as (ts, seq, type, bids, asks)."""
    return [(e["ts_ns"], e["seq"], e["is_snapshot"],
             tuple(sorted(e["bids"])), tuple(sorted(e["asks"])))
            for e in _read_events(tape)]


def _states_by_seq_in_stored_order(tape: Path) -> dict:
    """Replay the tape in the order DataReader streams it, without any
    re-sort, and key the ladder after each event by its seq."""
    headers, levels = lrvx.DataReader(str(tape)).read_book_updates()
    bids: dict = {}
    asks: dict = {}
    out = {}
    for h in headers:
        orderbook._apply_event(bids, asks,
                               event_type=int(h["event_type"]),
                               n_bid=int(h["bid_count"]),
                               n_ask=int(h["ask_count"]),
                               off=int(h["level_offset"]),
                               levels=levels)
        out[int(h["seq"])] = (tuple(sorted(bids.items())),
                              tuple(sorted(asks.items())))
    return out


def _book_at(tape: Path, ts_ns: int):
    snap = orderbook.book_at(tape, ts_ns=ts_ns, levels=0, symbol_id=_SYM)
    if snap is None:
        return None
    return (tuple(snap.bids), tuple(snap.asks))


class T1MultiDayTests(unittest.TestCase):
    book_type = "t1"

    def _days(self) -> list:
        return _t1_days()

    def _row_ts_ms(self, row) -> int:
        return row[5]

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix=f"binance-{self.book_type}-days-"))
        self.mirror = self.tmp / "mirror"
        self.tape = self.tmp / "tape"
        self.days = self._days()
        self.paths = [
            _write_zip(bnc._book_mirror_path(self.mirror, "BTCUSDT", "um-futures",
                                             self.book_type, date(2024, 1, 15 + i)),
                       rows)
            for i, rows in enumerate(self.days)
        ]

    def _convert(self, path: Path, tape: Path) -> bnc.BookConvertStats:
        return bnc.t1_to_lrvx(path, tape, symbol_id=_SYM,
                              symbol_name="BTCUSDT", market="um-futures")

    def _import(self, day: int) -> bnc.BookConvertStats:
        return self._convert(self.paths[day], self.tape)

    def _range(self, first: int, last: int) -> bnc.BookConvertStats:
        return bnc.range_book_to_lrvx(
            "BTCUSDT", "um-futures", self.book_type,
            date(2024, 1, 15 + first).isoformat(),
            date(2024, 1, 15 + last).isoformat(),
            self.tape, mirror=self.mirror, parallel=1, symbol_id=_SYM)

    def _reference(self, *days: int) -> Path:
        """One in-order import of ``days`` into an empty tape."""
        ref = self.tmp / ("ref-" + "-".join(map(str, days)))
        if not ref.exists():
            for d in sorted(days):
                self._convert(self.paths[d], ref)
        return ref

    def _probe_times(self) -> list:
        ts = {self._row_ts_ms(r) * 1_000_000
              for rows in self.days for r in rows}
        between = {(_DAY0_MS + d * _DAY_MS) * 1_000_000 - 1
                   for d in range(_N_DAYS + 1)}
        return sorted(ts | between)

    def assertSameTape(self, *days: int) -> None:
        ref = self._reference(*days)
        got = _records(self.tape)
        self.assertEqual(len(got), len(set(got)), "a record is on the tape twice")
        self.assertEqual(sorted(got), sorted(_records(ref)))
        for t in self._probe_times():
            self.assertEqual(_book_at(self.tape, t), _book_at(ref, t),
                             f"book_at({t}) differs")
        self.assertEqual(_states_by_seq_in_stored_order(self.tape),
                         _states_by_seq_in_stored_order(ref))

    def test_reference_matches_source(self) -> None:
        ref = self._reference(*range(_N_DAYS))
        self.assertTrue(_records(ref))
        # The top in force at each probe time is the last source row
        # at or before it.
        rows = [r for day in self.days for r in day]
        for t in self._probe_times():
            before = [r for r in rows if r[5] * 1_000_000 <= t]
            if not before:
                self.assertIsNone(_book_at(ref, t))
                continue
            r = before[-1]
            self.assertEqual(_book_at(ref, t),
                             (((r[1], r[2]),), ((r[3], r[4]),)))

    def test_two_days_in_sequence(self) -> None:
        self._import(0)
        second = self._import(1)
        self.assertGreater(second.snapshots_written + second.deltas_written, 0)
        self.assertSameTape(0, 1)

    def test_reimport_adds_nothing(self) -> None:
        self._import(0)
        self._import(1)
        for day in (1, 0):
            again = self._import(day)
            self.assertEqual(again.snapshots_written, 0)
            self.assertEqual(again.deltas_written, 0)
            self.assertEqual(again.rows_skipped, again.rows_read)
        self.assertSameTape(0, 1)

    def test_overlapping_range(self) -> None:
        self._range(0, 1)
        self._range(1, 2)
        self.assertSameTape(0, 1, 2)

    def test_out_of_order_days(self) -> None:
        for day in (2, 0, 1):
            stats = self._import(day)
            self.assertEqual(stats.snapshots_written, 1)
        self.assertSameTape(0, 1, 2)

    def test_gap_fill(self) -> None:
        self._import(0)
        self._import(2)
        middle = self._import(1)
        self.assertEqual(middle.snapshots_written, 1)
        self.assertSameTape(0, 1, 2)

    def test_day_cut_short_is_completed(self) -> None:
        # The first half of a day on the tape (an interrupted import),
        # then the whole day: the missing half lands, nothing twice.
        head = _write_zip(self.tmp / "head" / "BTCUSDT-head.zip",
                          self.days[1][: len(self.days[1]) // 2])
        self._import(0)
        self._convert(head, self.tape)
        self._import(1)
        self.assertSameTape(0, 1)

    def test_dedup_read_is_bounded_to_the_incoming_day(self) -> None:
        self._import(0)
        patcher, calls = _record_reader_ranges()
        with patcher:
            self._import(1)
        self.assertTrue(calls)
        ts = [self._row_ts_ms(r) * 1_000_000 for r in self.days[1]]
        for kwargs in calls:
            self.assertEqual(kwargs.get("from_ns"), min(ts))
            self.assertEqual(kwargs.get("to_ns"), max(ts))
            self.assertEqual(kwargs.get("symbols"), [_SYM])


@unittest.skipUnless(net.ENABLED, net.SKIP_REASON)
class T1RealArchiveTests(unittest.TestCase):
    """A real bookTicker day, downloaded from data.binance.vision: every
    row parses, and the top of book after the first, a middle and the
    last record equals the CSV row of that update id."""

    def test_um_futures_day(self) -> None:
        with tempfile.TemporaryDirectory(prefix="binance-t1-net-") as tmp:
            tmp = Path(tmp)
            day = date(2024, 1, 15)
            stats = bnc.range_book_to_lrvx("ETHBTC", "um-futures", "t1", day, day,
                                           tmp / "tape", mirror=tmp / "mirror",
                                           parallel=1)
            self.assertGreater(stats.snapshots_written + stats.deltas_written, 0)
            self.assertEqual(stats.rows_invalid, 0)
            rows = net.csv_rows(bnc._book_mirror_path(tmp / "mirror", "ETHBTC",
                                                      "um-futures", "t1", day))
            self.assertEqual(",".join(rows[0]), _T1_HEADER)
            by_id = {int(r[0]): r for r in rows[1:]}
            events = _read_events(tmp / "tape")
            book: dict = {}
            for i, ev in enumerate(events):
                if ev["is_snapshot"]:
                    book = {"b": dict(ev["bids"]), "a": dict(ev["asks"])}
                else:
                    for side, levels in (("b", ev["bids"]), ("a", ev["asks"])):
                        for px, q in levels:
                            if q:
                                book[side][px] = q
                            else:
                                book[side].pop(px, None)
                if i in (0, len(events) // 2, len(events) - 1):
                    r = by_id[ev["seq"]]
                    self.assertEqual(ev["ts_ns"], int(r[5]) * 1_000_000)
                    self.assertEqual(book["b"], {round(float(r[1]) * 1e8): round(float(r[2]) * 1e8)})
                    self.assertEqual(book["a"], {round(float(r[3]) * 1e8): round(float(r[4]) * 1e8)})


if __name__ == "__main__":
    unittest.main()
