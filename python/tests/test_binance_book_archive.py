"""Tests for ``lrvx.archives.binance.{t1_to_lrvx, depth20_to_lrvx}``.

No network: synthetic bookTicker CSVs in the layout published on
``data.binance.vision`` and bookDepth CSVs in the long format
``depth20_to_lrvx`` documents are built in-memory and fed through the
converters. The resulting `.lrvx`
directory is read back via ``lrvx.DataReader.read_book_updates``
and asserted ladder-by-ladder.

Covers:

  * `t1_to_lrvx`: initial snapshot + delta on the next change, no-op
    on unchanged ticks, dedup on re-run by `update_id`.
  * `depth20_to_lrvx`: long-format snap → 20-level snapshot,
    subsequent updates → deltas with qty=0 removals + new prices.
  * Co-existence: aggTrades + book events sit in one tape, monotonic
    on `exchange_ts_ns` (the existing reader interleaves them).
  * `metadata.json`: `has_book_snapshots` / `has_book_deltas` /
    `total_book_updates` / `book_depth` populated by the writer.
  * Multi-day append for both products: days in sequence, re-imports,
    overlapping ranges, out-of-order days, a gap fill and a day cut
    short each give the tape one in-order import of the same days
    gives, and the dedup read covers only the incoming day's time range.

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

import lrvx  # noqa: E402

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


def _build_t1_zip(dest: Path) -> Path:
    buf = io.StringIO()
    for r in _T1_ROWS:
        buf.write(",".join(str(x) for x in r) + "\n")
    with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(dest.with_suffix(".csv").name, buf.getvalue())
    return dest


def _build_depth20_zip(dest: Path) -> Path:
    """bookDepth long format: symbol, ts_ms, first_id, last_id, side,
    update_type, price, qty.

    First group (last_update_id=10) is the snapshot — 3 bids + 3 asks.
    Second group (last_update_id=11) is an update: top bid removed,
    new top bid added, top ask qty changed.
    """
    rows = [
        ("BTCUSDT", 1_700_000_000_000, 1, 10, "b", "snap", 42_000.0, 1.0),
        ("BTCUSDT", 1_700_000_000_000, 1, 10, "b", "snap", 41_999.0, 1.5),
        ("BTCUSDT", 1_700_000_000_000, 1, 10, "b", "snap", 41_998.0, 2.0),
        ("BTCUSDT", 1_700_000_000_000, 1, 10, "a", "snap", 42_001.0, 1.0),
        ("BTCUSDT", 1_700_000_000_000, 1, 10, "a", "snap", 42_002.0, 1.5),
        ("BTCUSDT", 1_700_000_000_000, 1, 10, "a", "snap", 42_003.0, 2.0),
        ("BTCUSDT", 1_700_000_001_000, 11, 11, "b", "set",  42_000.0, 0.0),
        ("BTCUSDT", 1_700_000_001_000, 11, 11, "b", "set",  42_000.5, 0.8),
        ("BTCUSDT", 1_700_000_001_000, 11, 11, "a", "set",  42_001.0, 0.5),
    ]
    buf = io.StringIO()
    for r in rows:
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


class Depth20Tests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="binance-d20-"))
        self.zip_path = self.tmp / "BTCUSDT-bookDepth-2024-01-15.zip"
        self.tape = self.tmp / "tape"

    def test_snapshot_then_delta(self) -> None:
        _build_depth20_zip(self.zip_path)
        stats = bnc.depth20_to_lrvx(
            self.zip_path, self.tape, levels=20,
            symbol_id=7, symbol_name="BTCUSDT",
        )
        self.assertEqual(stats.snapshots_written, 1)
        self.assertEqual(stats.deltas_written, 1)

        events = _read_events(self.tape)
        self.assertEqual(len(events), 2)
        # Snapshot: 3 bids + 3 asks.
        self.assertTrue(events[0]["is_snapshot"])
        self.assertEqual(len(events[0]["bids"]), 3)
        self.assertEqual(len(events[0]["asks"]), 3)
        # Delta: top bid removed, new top bid added, top ask changed.
        self.assertFalse(events[1]["is_snapshot"])
        bids = dict(events[1]["bids"])
        asks = dict(events[1]["asks"])
        # 42_000.0 → qty 0 (removal); 42_000.5 → 0.8 (new).
        self.assertEqual(bids[4_200_000_000_000], 0)
        self.assertEqual(bids[4_200_050_000_000], 80_000_000)
        # 42_001.0 ask qty changed 1.0 → 0.5.
        self.assertEqual(asks[4_200_100_000_000], 50_000_000)

    def test_metadata_book_fields(self) -> None:
        _build_depth20_zip(self.zip_path)
        bnc.depth20_to_lrvx(self.zip_path, self.tape, levels=20,
                               symbol_id=7, symbol_name="BTCUSDT")
        meta = json.loads((self.tape / "metadata.json").read_text())
        self.assertTrue(meta["has_book_snapshots"])
        self.assertTrue(meta["has_book_deltas"])
        self.assertEqual(meta["total_book_updates"], 2)
        self.assertEqual(meta["book_depth"], 20)


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
        # (agg_id, price, qty, first_id, last_id, ts_ms, maker, best)
        for i in range(3):
            buf.write(f"{1000+i},42000.0,0.01,{i},{i},"
                      f"{1_700_000_000_500 + i * 1000},True,True\n")
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


def _depth20_days() -> list:
    """bookDepth long-format rows for _N_DAYS days: each day opens
    with a ``snap`` group of the ladder the previous day left, then
    single-id ``set`` groups that add, change and remove levels."""
    rng = random.Random(23)
    days = []
    uid = 900_000
    bids = {42_000.0 - i: 1.0 + i for i in range(5)}
    asks = {42_001.0 + i: 1.0 + i for i in range(5)}
    for d in range(_N_DAYS):
        rows = []
        ts = _DAY0_MS + d * _DAY_MS + 40
        uid += 1
        for p, q in sorted(bids.items(), reverse=True):
            rows.append(("BTCUSDT", ts, uid, uid, "b", "snap", p, q))
        for p, q in sorted(asks.items()):
            rows.append(("BTCUSDT", ts, uid, uid, "a", "snap", p, q))
        for _ in range(40):
            uid += rng.randint(1, 3)
            ts += rng.choice((1, 250, 5_000))
            for _ in range(rng.randint(1, 3)):
                side = rng.choice(("b", "a"))
                book = bids if side == "b" else asks
                if len(book) > 2 and rng.random() < 0.3:
                    p, q = rng.choice(sorted(book)), 0.0
                    del book[p]
                else:
                    best = max(bids) if side == "b" else min(asks)
                    off = rng.choice((0.0, 1.0, 2.0, 3.0))
                    p = best - off if side == "b" else best + off
                    q = rng.choice((0.5, 1.25, 3.0))
                    book[p] = q
                rows.append(("BTCUSDT", ts, uid, uid, side, "set", p, q))
        days.append(rows)
    return days


def _write_zip(dest: Path, rows) -> Path:
    dest.parent.mkdir(parents=True, exist_ok=True)
    buf = io.StringIO()
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


class _MultiDayBookAppend:
    """Scenarios shared by both products; subclasses pick the product."""

    book_type = ""

    def _days(self) -> list:
        raise NotImplementedError

    def _row_ts_ms(self, row) -> int:
        raise NotImplementedError

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
        convert = bnc.t1_to_lrvx if self.book_type == "t1" else bnc.depth20_to_lrvx
        return convert(path, tape, symbol_id=_SYM, symbol_name="BTCUSDT",
                       market="um-futures")

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
        if self.book_type != "t1":
            return
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


class T1MultiDayTests(_MultiDayBookAppend, unittest.TestCase):
    book_type = "t1"

    def _days(self) -> list:
        return _t1_days()

    def _row_ts_ms(self, row) -> int:
        return row[5]


class Depth20MultiDayTests(_MultiDayBookAppend, unittest.TestCase):
    book_type = "depth20"

    def _days(self) -> list:
        return _depth20_days()

    def _row_ts_ms(self, row) -> int:
        return row[1]


if __name__ == "__main__":
    unittest.main()
