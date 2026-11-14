"""Day conversion of the archive range importers runs in worker processes.

``range_to_lrvx`` (Binance aggTrades, Bybit, OKX, Bitget) and
``binance.range_book_to_lrvx`` take ``workers``: how many processes
convert the range's files. ``None`` (the default) is one per core,
capped at the number of files; ``1`` converts in the calling process,
as before; anything below 1 is a ValueError naming ``workers``, raised
before any download. ``parallel`` still counts concurrent downloads
only. The CLI passes ``--workers N`` through.

The work is spread by ``lrvx.archives._range.convert_days(items,
convert_one, *, workers)``: ``convert_one`` (a picklable, module-level
function) runs on every item in up to ``workers`` processes, the
results come back in item order, ``workers == 1`` stays in the calling
process, and an exception raised by ``convert_one`` is re-raised in the
caller with its own type and message.

What the parallel path must give is exactly the serial result:

  * the same records in the same stored order, every field;
  * the same segments: as many, each with the same first / last event,
    event count, index flag and size (segment file names carry the wall
    clock and so do not compare);
  * the same ``metadata.json`` and the same returned stats, field by
    field;
  * the same append semantics into an existing tape: a day already on
    the tape is skipped in full, a gap day lands in full.

A day that fails to convert (a header the exchange does not publish, a
cached archive that does not open) raises the error the serial import
raises, naming the file, without hanging, and a parallel import that
fails writes nothing: a fresh tape gets no segment and no metadata, a
tape that existed is left as it was.

Run from repo root:
    PYTHONPATH=build/python python3 -m pytest python/tests/test_archive_parallel_conversion.py
"""
from __future__ import annotations

import gzip
import io
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest
import uuid
import zipfile
from contextlib import redirect_stdout
from dataclasses import dataclass, field
from datetime import date, timedelta
from pathlib import Path
from typing import Callable, List, Optional
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[2]
for cand in ("build/python", "build-py312/python"):
    p = REPO_ROOT / cand
    if p.is_dir():
        sys.path.insert(0, str(p))
        break
TESTS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TESTS_DIR))

import lrvx  # noqa: E402

from support import archive_workers  # noqa: E402
from lrvx import cli  # noqa: E402
from lrvx.archives import _dedup, _range  # noqa: E402
from lrvx.archives import binance as bnc  # noqa: E402
from lrvx.archives import bitget, bybit, okx  # noqa: E402
from lrvx.archives._download import CorruptArchiveError  # noqa: E402


_SYM = 7
_DAY0 = date(2024, 1, 15)
_N_DAYS = 5
_DAY_MS = 86_400_000
# A parallel import that has not finished by then is taken to hang.
_HANG_S = 120


def _day(i: int) -> date:
    return _DAY0 + timedelta(days=i)


def _shuffled(rows: list, seed: int) -> list:
    out = list(rows)
    random.Random(seed).shuffle(out)
    return out


def _write_zip(dest: Path, text: str) -> Path:
    dest.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(dest.with_suffix(".csv").name, text)
    return dest


def _write_gz(dest: Path, text: str) -> Path:
    dest.parent.mkdir(parents=True, exist_ok=True)
    with gzip.open(dest, "wt", encoding="utf-8") as f:
        f.write(text)
    return dest


# ── One importer, described once ─────────────────────────────────────


@dataclass
class Case:
    """How to build a synthetic mirror for one importer and call its
    range function on it."""

    name: str
    module: object
    func: str
    lead: tuple
    # rows of day i, in the order the file lists them
    day_rows: Callable[[int], list]
    # writes day i's rows into the mirror, returns the files written
    write_day: Callable[[Path, int, list], List[Path]]
    # writes one file's worth of text in the importer's container format
    container: Callable[[Path, str], Path]
    # converts one file into a tape with the single-file function
    single: Callable[[Path, Path], object]
    book: bool = False
    cli: Optional[List[str]] = None
    files_per_day: Callable[[int], int] = field(default=lambda i: 1)

    def range(self, mirror: Path, tape: Path, first: int, last: int, **kw):
        fn = getattr(self.module, self.func)
        return fn(*self.lead, _day(first).isoformat(), _day(last).isoformat(),
                  tape, mirror=mirror, parallel=1, symbol_id=_SYM, **kw)

    def spec(self, mirror: Path, tape: Path, first: int, last: int,
             **kw) -> dict:
        return {"module": self.module.__name__, "func": self.func,
                "args": [*self.lead, _day(first).isoformat(),
                         _day(last).isoformat(), str(tape)],
                "kwargs": {"mirror": str(mirror), "parallel": 1,
                           "symbol_id": _SYM, **kw}}

    def build(self, mirror: Path) -> List[List[Path]]:
        return [self.write_day(mirror, i, self.day_rows(i))
                for i in range(_N_DAYS)]


# Binance aggTrades: file order is kept, ids rise with time.

_BNC_HEADER = ("agg_trade_id,price,quantity,first_trade_id,last_trade_id,"
               "transact_time,is_buyer_maker")


def _bnc_rows(i: int) -> list:
    start = 1_705_276_800_000 + i * _DAY_MS
    base = 50_000 + i * 1_000
    return [(base + k, 42_000.0 + k * 0.5, 0.001 * (k + 1), base + k, base + k,
             start + (k // 2) * 30_000, "true" if k % 3 else "false")
            for k in range(120)]


def _bnc_write(mirror: Path, i: int, rows: list) -> List[Path]:
    text = _BNC_HEADER + "\n" + "".join(
        ",".join(str(x) for x in r) + "\n" for r in rows)
    dest = bnc._mirror_path(mirror, "BTCUSDT", "um-futures", _day(i))
    return [_write_zip(dest, text)]


BINANCE = Case(
    name="binance", module=bnc, func="range_to_lrvx",
    lead=("BTCUSDT", "um-futures"), day_rows=_bnc_rows, write_day=_bnc_write,
    container=_write_zip,
    single=lambda path, tape: bnc.aggtrades_to_lrvx(
        path, tape, symbol_id=_SYM, symbol_name="BTCUSDT", market="um-futures"),
    cli=["archive", "binance", "--symbol", "BTCUSDT", "--market", "um-futures"],
)


# Binance bookTicker (t1).

_T1_HEADER = ("update_id,best_bid_price,best_bid_qty,best_ask_price,"
              "best_ask_qty,transaction_time,event_time")


def _t1_rows(i: int) -> list:
    rng = random.Random(100 + i)
    uid = 3_831_603_310_000 + i * 10_000
    bid = 42_000.0
    ts = 1_705_276_800_000 + i * _DAY_MS + 15
    rows = []
    for _ in range(80):
        uid += rng.randint(1, 5)
        ts += rng.choice((0, 0, 7, 1_000, 60_000))
        if rng.random() < 0.7:
            bid += rng.choice((-1.0, -0.5, 0.0, 0.5, 1.0))
        rows.append((uid, bid, rng.choice((0.5, 1.0, 1.5)), bid + 0.5,
                     rng.choice((0.25, 2.0)), ts, ts + 4))
    return rows


def _t1_write(mirror: Path, i: int, rows: list) -> List[Path]:
    text = _T1_HEADER + "\n" + "".join(
        ",".join(str(x) for x in r) + "\n" for r in rows)
    dest = bnc._book_mirror_path(mirror, "BTCUSDT", "um-futures", "t1", _day(i))
    return [_write_zip(dest, text)]


BINANCE_BOOK = Case(
    name="binance-book", module=bnc, func="range_book_to_lrvx",
    lead=("BTCUSDT", "um-futures", "t1"), day_rows=_t1_rows,
    write_day=_t1_write, container=_write_zip,
    single=lambda path, tape: bnc.t1_to_lrvx(
        path, tape, symbol_id=_SYM, symbol_name="BTCUSDT", market="um-futures"),
    book=True,
    cli=["archive", "binance", "--book", "t1", "--symbol", "BTCUSDT",
         "--market", "um-futures"],
)


# Bybit linear: UUID trade ids (folded, unordered), a day listed newest
# first as older files are, a shuffled day and timestamp ties.

_BYBIT_HEADER = ("timestamp,symbol,side,size,price,tickDirection,trdMatchID,"
                 "grossValue,homeNotional,foreignNotional")


def _bybit_rows(i: int, n: int = 150) -> list:
    start = 1_705_276_800 + i * 86_400
    rows = []
    for k in range(n):
        trd = str(uuid.uuid5(uuid.NAMESPACE_OID, f"parallel-{i}-{k}"))
        qty, price = 0.001 * (k % 50 + 1), 42_000.0 + (k % 97)
        rows.append((f"{start + (k // 3) * 7}.{k % 3}25", "BTCUSDT",
                     "Buy" if k % 2 == 0 else "Sell", qty, price, "PlusTick",
                     trd, f"{qty * price * 1e8!r}", qty, qty * price))
    if i == 1:
        rows.reverse()
    elif i == 3:
        rows = _shuffled(rows, seed=i)
    return rows


def _bybit_text(rows: list) -> str:
    return _BYBIT_HEADER + "\n" + "".join(
        ",".join(str(x) for x in r) + "\n" for r in rows)


def _bybit_write(mirror: Path, i: int, rows: list) -> List[Path]:
    dest = bybit._mirror_path(mirror, "BTCUSDT", "linear", _day(i))
    return [_write_gz(dest, _bybit_text(rows))]


BYBIT = Case(
    name="bybit", module=bybit, func="range_to_lrvx",
    lead=("BTCUSDT", "linear"), day_rows=_bybit_rows, write_day=_bybit_write,
    container=_write_gz,
    single=lambda path, tape: bybit.trades_to_lrvx(
        path, tape, symbol_id=_SYM, symbol_name="BTCUSDT", market="linear"),
    cli=["archive", "bybit", "--symbol", "BTCUSDT", "--market", "linear"],
)


# Bybit spot: millisecond timestamps, ids counting from 1 every day.

def _bybit_spot_rows(i: int) -> list:
    start = 1_705_276_800_000 + i * _DAY_MS
    rows = [(k + 1, start + (k // 2) * 1_500, 0.45 + (k % 13) * 0.0001,
             round(10.0 + k * 0.25, 2), "buy" if k % 3 else "sell")
            for k in range(130)]
    return list(reversed(rows)) if i == 2 else rows


def _bybit_spot_write(mirror: Path, i: int, rows: list) -> List[Path]:
    text = "id,timestamp,price,volume,side\n" + "".join(
        ",".join(str(x) for x in r) + "\n" for r in rows)
    dest = bybit._mirror_path(mirror, "MNTUSDT", "spot", _day(i))
    return [_write_gz(dest, text)]


BYBIT_SPOT = Case(
    name="bybit-spot", module=bybit, func="range_to_lrvx",
    lead=("MNTUSDT", "spot"), day_rows=_bybit_spot_rows,
    write_day=_bybit_spot_write, container=_write_gz,
    single=lambda path, tape: bybit.trades_to_lrvx(
        path, tape, symbol_id=_SYM, symbol_name="MNTUSDT", market="spot"),
)


# OKX swap: UTC+8 days, a shuffled day.

_OKX_HEADER = "instrument_name,trade_id,side,price,size,created_time"


def _okx_rows(i: int) -> list:
    start = 1_705_248_000_000 + i * _DAY_MS
    base = 720_073_478 + i * 1_000
    rows = [(base + k, "buy" if k % 2 == 0 else "sell", float(k % 7 + 1),
             42_897.9 + (k % 11) * 0.1, start + (k // 2) * 450)
            for k in range(140)]
    return _shuffled(rows, seed=50 + i) if i == 2 else rows


def _okx_write(mirror: Path, i: int, rows: list) -> List[Path]:
    text = "\n".join([_OKX_HEADER] + [
        f"BTC-USDT-SWAP,{t},{s},{p},{q},{ts}" for t, s, q, p, ts in rows]) + "\n"
    dest = okx._mirror_path(mirror, "BTC-USDT-SWAP", "swap", _day(i))
    return [_write_zip(dest, text)]


OKX = Case(
    name="okx", module=okx, func="range_to_lrvx",
    lead=("BTC-USDT-SWAP", "swap"), day_rows=_okx_rows, write_day=_okx_write,
    container=_write_zip,
    single=lambda path, tape: okx.trades_to_lrvx(
        path, tape, symbol_id=_SYM, symbol_name="BTC-USDT-SWAP", market="swap"),
    cli=["archive", "okx", "--symbol", "BTC-USDT-SWAP", "--market", "swap"],
)


# Bitget umcbl: UTC+8 days, day 1 published in two parts, a shuffled day.

_BITGET_HEADER = "trade_id,timestamp,price,side,volume(quote),size(base)"


def _bitget_rows(i: int) -> list:
    start = 1_705_248_000_000 + i * _DAY_MS
    base = 1_130_457_536_508_936_193 + i * 1_000
    rows = [(base + k, start + (k // 2) * 600, 42_000.0 + (k % 17) * 0.1,
             "buy" if k % 2 == 0 else "sell", 0.0001 * (k % 23 + 1))
            for k in range(110)]
    return _shuffled(rows, seed=70 + i) if i == 3 else rows


def _bitget_text(rows: list) -> str:
    return _BITGET_HEADER + "\n" + "".join(
        f"{t},{ts},{p},{s},{p * q!r},{q}\n" for t, ts, p, s, q in rows)


def _bitget_write(mirror: Path, i: int, rows: list) -> List[Path]:
    parts = [rows[:60], rows[60:]] if i == 1 else [rows]
    paths = [_write_zip(bitget._mirror_path(mirror, "BTCUSDT", "umcbl", _day(i),
                                            part=n + 1), _bitget_text(chunk))
             for n, chunk in enumerate(parts)]
    bitget._manifest_path(mirror, "BTCUSDT", "umcbl", _day(i)).write_text(
        "\n".join(p.name for p in paths) + "\n")
    return paths


BITGET = Case(
    name="bitget", module=bitget, func="range_to_lrvx",
    lead=("BTCUSDT", "umcbl"), day_rows=_bitget_rows, write_day=_bitget_write,
    container=_write_zip,
    single=lambda path, tape: bitget.trades_to_lrvx(
        path, tape, symbol_id=_SYM, symbol_name="BTCUSDT", market="umcbl"),
    cli=["archive", "bitget", "--symbol", "BTCUSDT", "--market", "umcbl"],
    files_per_day=lambda i: 2 if i == 1 else 1,
)


# ── What a tape holds, in a form two tapes can be compared by ────────


def tape_contents(tape: Path) -> Optional[dict]:
    """Every record in stored order, the segments in file-name order
    (first / last event, event count, index flag, size) and the parsed
    metadata.json; None for a tape that does not exist."""
    if not tape.exists():
        return None
    reader = lrvx.DataReader(str(tape), order_segments_by_time=False)
    headers, levels = reader.read_book_updates()
    segments = sorted(reader.segments(), key=lambda s: Path(s["path"]).name)
    meta_path = tape / "metadata.json"
    return {
        "trades": reader.read_trades().tolist(),
        "book_headers": headers.tolist(),
        "book_levels": levels.tolist(),
        "segments": [(s["first_event_ns"], s["last_event_ns"], s["event_count"],
                      s["has_index"], (tape / Path(s["path"]).name).stat().st_size)
                     for s in segments],
        "metadata": (json.loads(meta_path.read_text())
                     if meta_path.exists() else None),
    }


def tape_files(tape: Path) -> list:
    """Name, size and bytes of every file in a tape directory: what must
    not move when an import into it fails."""
    if not tape.exists():
        return []
    return sorted((p.relative_to(tape).as_posix(), p.read_bytes())
                  for p in tape.rglob("*") if p.is_file())


def written_files(tape: Path) -> list:
    if not tape.exists():
        return []
    return sorted(p.name for p in tape.rglob("*")
                  if p.is_file() and (p.suffix == ".lrvx" or p.name == "metadata.json"))


def run_isolated(spec: dict) -> dict:
    """Run one range import in its own process; fail the test if it has
    not returned within _HANG_S seconds."""
    env = dict(os.environ)
    build = next((str(REPO_ROOT / c) for c in ("build/python", "build-py312/python")
                  if (REPO_ROOT / c).is_dir()), "")
    env["PYTHONPATH"] = os.pathsep.join(
        x for x in (build, str(TESTS_DIR), env.get("PYTHONPATH", "")) if x)
    try:
        r = subprocess.run(
            [sys.executable, str(TESTS_DIR / "support" / "archive_workers.py"),
             json.dumps(spec)],
            capture_output=True, text=True, timeout=_HANG_S, cwd=REPO_ROOT, env=env)
    except subprocess.TimeoutExpired:
        raise AssertionError(
            f"the import did not return within {_HANG_S} s: a failed worker "
            f"must surface as an error, not a hang") from None
    lines = [ln for ln in r.stdout.splitlines() if ln.startswith("{")]
    if r.returncode != 0 or not lines:
        raise AssertionError(f"isolated import crashed (rc={r.returncode}):\n"
                             f"{r.stdout}\n{r.stderr}")
    return json.loads(lines[-1])


# ── The importers, one test class each ───────────────────────────────


class _ParallelConversionContract:
    """Mixed into one TestCase per importer; ``case`` says which."""

    case: Case

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix=f"parallel-{self.case.name}-"))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.mirror = self.tmp / "mirror"
        self.files = self.case.build(self.mirror)
        self.rows = [self.case.day_rows(i) for i in range(_N_DAYS)]

    # helpers

    def _range(self, tape: Path, first: int = 0, last: int = _N_DAYS - 1, **kw):
        return self.case.range(self.mirror, tape, first, last, **kw)

    def assertSameImport(self, serial_stats, serial_tape: Path,
                         other_stats, other_tape: Path) -> None:
        self.assertEqual(vars(other_stats), vars(serial_stats),
                         "the returned stats differ from the serial import's")
        serial, other = tape_contents(serial_tape), tape_contents(other_tape)
        self.assertIsNotNone(serial)
        self.assertIsNotNone(other)
        for key in ("segments", "metadata", "trades", "book_headers",
                    "book_levels"):
            self.assertEqual(other[key], serial[key],
                             f"{key} differ from the serial import's")

    def _assert_nontrivial(self, stats, tape: Path, n_files: int) -> None:
        contents = tape_contents(tape)
        records = (len(contents["book_headers"]) if self.case.book
                   else len(contents["trades"]))
        self.assertGreater(records, 0)
        self.assertEqual(stats.files_processed, n_files)

    def _bad_day(self, index: int = 2) -> Path:
        return self.files[index][0]

    # equivalence

    def test_parallel_matches_serial(self) -> None:
        serial = self._range(self.tmp / "serial", workers=1)
        parallel = self._range(self.tmp / "parallel", workers=4)
        self._assert_nontrivial(serial, self.tmp / "serial",
                                sum(len(f) for f in self.files))
        self.assertSameImport(serial, self.tmp / "serial",
                              parallel, self.tmp / "parallel")

    def test_append_into_existing_tape_matches_serial(self) -> None:
        # Days 0 and 2 are on the tape already; the range is days 1..4, so
        # day 2 is skipped in full and day 1 fills the gap before it.
        base = self.tmp / "base"
        for i in (0, 2):
            for path in self.files[i]:
                self.case.single(path, base)
        serial_tape, parallel_tape = self.tmp / "serial", self.tmp / "parallel"
        shutil.copytree(base, serial_tape)
        shutil.copytree(base, parallel_tape)

        serial = self._range(serial_tape, 1, _N_DAYS - 1, workers=1)
        parallel = self._range(parallel_tape, 1, _N_DAYS - 1, workers=4)
        self.assertSameImport(serial, serial_tape, parallel, parallel_tape)

        self.assertGreater(parallel.rows_skipped, 0)
        if not self.case.book:
            self.assertEqual(parallel.rows_skipped, len(self.rows[2]))
            self.assertEqual(parallel.trades_written,
                             sum(len(self.rows[i]) for i in (1, 3, 4)))
            # Bybit spot ids restart every day, so a trade is its id and time.
            keys = [(int(t["trade_id"]), int(t["exchange_ts_ns"])) for t in
                    lrvx.DataReader(str(parallel_tape)).read_trades()]
            self.assertEqual(len(keys), len(set(keys)))
            self.assertEqual(len(keys), sum(len(r) for r in self.rows))

    # the knob

    def test_conversion_goes_through_convert_days(self) -> None:
        with mock.patch.object(_range, "convert_days",
                               wraps=_range.convert_days) as spy:
            self._range(self.tmp / "tape", workers=3)
        self.assertEqual(spy.call_count, 1,
                         "a range is converted in one convert_days call")
        call = spy.call_args
        items = call.args[0] if call.args else call.kwargs["items"]
        self.assertEqual(len(list(items)), sum(len(f) for f in self.files),
                         "every file of the range is one item")
        self.assertEqual(call.kwargs.get("workers"), 3)

    def test_default_workers_is_one_per_core_capped_at_the_files(self) -> None:
        n_files = sum(len(f) for f in self.files)
        for cores, expected in ((2, 2), (64, n_files)):
            tape = self.tmp / f"tape-{cores}"
            with mock.patch.object(os, "cpu_count", return_value=cores), \
                    mock.patch.object(_range, "convert_days",
                                      wraps=_range.convert_days) as spy:
                self._range(tape)
            self.assertEqual(spy.call_args.kwargs.get("workers"), expected,
                             f"{cores} cores, {n_files} files")

    def test_workers_below_one_is_refused_before_any_download(self) -> None:
        empty = self.tmp / "empty-mirror"
        with mock.patch.object(self.case.module, "_download",
                               side_effect=AssertionError("download attempted")):
            for bad in (0, -1):
                with self.assertRaises(ValueError) as ctx:
                    self.case.range(empty, self.tmp / "tape", 0, _N_DAYS - 1,
                                    workers=bad)
                self.assertIn("workers", str(ctx.exception))
        self.assertEqual(written_files(self.tmp / "tape"), [])

    # failures

    def test_unknown_layout_day_fails_like_serial_and_leaves_the_tape(self) -> None:
        bad = self._bad_day()
        self.case.container(bad, "id,px\n1,2\n")

        with self.assertRaises(ValueError) as serial_err:
            self._range(self.tmp / "serial", workers=1)

        # Into a tape that already holds day 0: it must come out untouched.
        tape = self.tmp / "existing"
        for path in self.files[0]:
            self.case.single(path, tape)
        before = tape_files(tape)
        out = run_isolated(self.case.spec(self.mirror, tape, 1, _N_DAYS - 1,
                                          workers=4))
        self.assertFalse(out["ok"], "the import of a range with a bad day succeeded")
        self.assertEqual(out["type"], type(serial_err.exception).__name__)
        self.assertEqual(out["type"], "UnknownLayoutError")
        self.assertIn(bad.name, out["message"])
        self.assertEqual(tape_files(tape), before,
                         "a failed parallel import changed the existing tape")

    def test_corrupt_day_fails_like_serial_evicts_it_and_writes_nothing(self) -> None:
        bad = self._bad_day()
        bad.write_bytes(b"this is not an archive")
        with self.assertRaises(CorruptArchiveError) as serial_err:
            self._range(self.tmp / "serial", workers=1)
        serial_type = type(serial_err.exception).__name__
        self.assertEqual(serial_type, "CorruptArchiveError")
        self.assertFalse(bad.exists())

        bad.write_bytes(b"this is not an archive")
        tape = self.tmp / "fresh"
        out = run_isolated(self.case.spec(self.mirror, tape, 0, _N_DAYS - 1,
                                          workers=4))
        self.assertFalse(out["ok"], "the import of a range with a bad day succeeded")
        self.assertEqual(out["type"], serial_type)
        self.assertIn(bad.name, out["message"])
        self.assertFalse(bad.exists(), "the corrupt cached file was not evicted")
        self.assertEqual(written_files(tape), [],
                         "a failed parallel import left a partial tape")


class BinanceAggTradesParallelTests(_ParallelConversionContract, unittest.TestCase):
    case = BINANCE


class BinanceBookParallelTests(_ParallelConversionContract, unittest.TestCase):
    case = BINANCE_BOOK


class BybitLinearParallelTests(_ParallelConversionContract, unittest.TestCase):
    case = BYBIT

    def test_default_workers_matches_serial(self) -> None:
        serial = self._range(self.tmp / "serial", workers=1)
        auto = self._range(self.tmp / "auto")
        self.assertSameImport(serial, self.tmp / "serial", auto, self.tmp / "auto")

    def test_serial_in_small_batches_matches_parallel(self) -> None:
        # The written order is a stable sort on the timestamp whatever the
        # batch size: a serial import merging many small sorted runs and a
        # parallel one give the same tape.
        with mock.patch.object(_dedup, "_BATCH_ROWS", 16):
            serial = self._range(self.tmp / "serial", workers=1)
        parallel = self._range(self.tmp / "parallel", workers=3)
        self.assertSameImport(serial, self.tmp / "serial",
                              parallel, self.tmp / "parallel")


class BybitSpotParallelTests(_ParallelConversionContract, unittest.TestCase):
    case = BYBIT_SPOT


class OkxSwapParallelTests(_ParallelConversionContract, unittest.TestCase):
    case = OKX


class BitgetParallelTests(_ParallelConversionContract, unittest.TestCase):
    case = BITGET


class BybitLargeDayParallelTests(unittest.TestCase):
    """Days longer than one parse batch (``_dedup._BATCH_ROWS``), one of
    them shuffled, so the workers spill and merge several sorted runs."""

    def test_multi_batch_days_match_serial(self) -> None:
        tmp = Path(tempfile.mkdtemp(prefix="parallel-bybit-large-"))
        self.addCleanup(shutil.rmtree, tmp, True)
        mirror = tmp / "mirror"
        n = _dedup._BATCH_ROWS + 700
        for i in range(3):
            rows = _bybit_rows(i, n)
            if i == 1:
                rows = _shuffled(rows, seed=99)
            _bybit_write(mirror, i, rows)
        serial = BYBIT.range(mirror, tmp / "serial", 0, 2, workers=1)
        parallel = BYBIT.range(mirror, tmp / "parallel", 0, 2, workers=3)
        self.assertEqual(serial.trades_written, 3 * n)
        self.assertEqual(vars(parallel), vars(serial))
        self.assertEqual(tape_contents(tmp / "parallel"),
                         tape_contents(tmp / "serial"))


# ── The shared helper ────────────────────────────────────────────────


def _within(seconds: float, fn):
    """Run ``fn`` in a thread; fail if it is still running after
    ``seconds``. Returns ``fn``'s result or re-raises its exception."""
    box: dict = {}

    def run() -> None:
        try:
            box["value"] = fn()
        except BaseException as exc:
            box["error"] = exc

    t = threading.Thread(target=run, daemon=True)
    t.start()
    t.join(seconds)
    if t.is_alive():
        raise AssertionError(f"still running after {seconds} s")
    if "error" in box:
        raise box["error"]
    return box["value"]


class ConvertDaysTests(unittest.TestCase):
    def test_one_worker_runs_in_the_calling_process(self) -> None:
        pids = _range.convert_days(list(range(4)), archive_workers.pid_of,
                                   workers=1)
        self.assertEqual(pids, [os.getpid()] * 4)

    def test_several_workers_run_in_other_processes(self) -> None:
        pids = _within(_HANG_S, lambda: _range.convert_days(
            list(range(6)), archive_workers.pid_of, workers=3))
        self.assertEqual(len(pids), 6)
        self.assertNotIn(os.getpid(), pids,
                         "conversion with workers=3 ran in the calling process")

    def test_results_come_back_in_item_order(self) -> None:
        items = list(range(12))
        out = _within(_HANG_S, lambda: _range.convert_days(
            items, archive_workers.tagged, workers=4))
        self.assertEqual(out, [archive_workers.tagged(i) for i in items])

    def test_no_items_gives_no_results(self) -> None:
        self.assertEqual(_range.convert_days([], archive_workers.tagged,
                                             workers=4), [])

    def test_a_failing_item_raises_its_own_error_in_the_caller(self) -> None:
        for workers in (1, 3):
            with self.subTest(workers=workers):
                with self.assertRaises(archive_workers.DayFailed) as ctx:
                    _within(_HANG_S, lambda: _range.convert_days(
                        list(range(6)), archive_workers.fail_on,
                        workers=workers))
                self.assertEqual(str(ctx.exception), "day 3 does not convert")


# ── The command line ─────────────────────────────────────────────────


_COMMANDS = ("binance", "bybit", "okx", "bitget")


def _subparser(command: str):
    import argparse

    def sub(parser, name):
        for action in parser._actions:
            if isinstance(action, argparse._SubParsersAction):
                return action.choices[name]
        raise KeyError(name)

    return sub(sub(cli._build_parser(), "archive"), command)


def _option_help(parser, option: str) -> Optional[str]:
    for action in parser._actions:
        if option in action.option_strings:
            return " ".join((action.help or "").split())
    return None


class CliWorkersTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="parallel-cli-"))
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def test_every_archive_command_takes_workers(self) -> None:
        parser = cli._build_parser()
        for command in _COMMANDS:
            with self.subTest(command=command):
                args = parser.parse_args(["archive", command, "--out", "t",
                                          "--workers", "3"])
                self.assertEqual(args.workers, 3)
                args = parser.parse_args(["archive", command, "--out", "t"])
                self.assertIsNone(args.workers,
                                  "the default is one worker per core")

    def test_help_says_what_workers_and_parallel_count(self) -> None:
        for command in _COMMANDS:
            with self.subTest(command=command):
                sub = _subparser(command)
                workers = _option_help(sub, "--workers")
                self.assertIsNotNone(workers, f"archive {command} has no --workers")
                self.assertIn("conver", workers.lower())
                self.assertIn("process", workers.lower())
                parallel = _option_help(sub, "--parallel")
                self.assertNotIn("serial", parallel.lower(),
                                 "--parallel still says conversion is serial")

    def test_workers_is_passed_through(self) -> None:
        commands = [
            (["archive", "binance", "--symbol", "BTCUSDT"], bnc,
             "range_to_lrvx", bnc.ConvertStats),
            (["archive", "binance", "--book", "t1", "--symbol", "BTCUSDT"], bnc,
             "range_book_to_lrvx", bnc.BookConvertStats),
            (["archive", "bybit", "--symbol", "BTCUSDT"], bybit,
             "range_to_lrvx", bybit.ConvertStats),
            (["archive", "okx", "--symbol", "BTC-USDT-SWAP"], okx,
             "range_to_lrvx", okx.ConvertStats),
            (["archive", "bitget", "--symbol", "BTCUSDT"], bitget,
             "range_to_lrvx", bitget.ConvertStats),
        ]
        for argv, module, func, stats in commands:
            for extra, expected in ((["--workers", "3"], 3), ([], None)):
                with self.subTest(argv=argv, extra=extra):
                    with mock.patch.object(module, func,
                                           return_value=stats()) as fn, \
                            redirect_stdout(io.StringIO()):
                        rc = cli.main(argv + ["--from", "2024-01-15",
                                              "--to", "2024-01-16",
                                              "--out", str(self.tmp / "t")]
                                      + extra)
                    self.assertEqual(rc, 0)
                    self.assertIn("workers", fn.call_args.kwargs)
                    self.assertEqual(fn.call_args.kwargs["workers"], expected)

    def test_cli_parallel_import_matches_serial(self) -> None:
        for case in (BYBIT, OKX):
            with self.subTest(case=case.name):
                mirror = self.tmp / case.name / "mirror"
                case.build(mirror)
                outputs = {}
                for workers in ("1", "2"):
                    tape = self.tmp / case.name / f"tape-{workers}"
                    buf = io.StringIO()
                    with redirect_stdout(buf):
                        rc = cli.main(case.cli + [
                            "--from", _day(0).isoformat(),
                            "--to", _day(_N_DAYS - 1).isoformat(),
                            "--mirror", str(mirror), "--out", str(tape),
                            "--symbol-id", str(_SYM), "--parallel", "1",
                            "--workers", workers])
                    self.assertEqual(rc, 0, buf.getvalue())
                    outputs[workers] = (buf.getvalue(), tape_contents(tape))
                self.assertIn("trades_written=", outputs["1"][0])
                self.assertEqual(outputs["2"][0], outputs["1"][0],
                                 "the summary line differs")
                self.assertEqual(outputs["2"][1], outputs["1"][1],
                                 "the tape differs")


if __name__ == "__main__":
    unittest.main()
