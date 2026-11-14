#!/usr/bin/env python3
"""Time a Bybit archive range import with one conversion worker and with
several.

Generates a synthetic Bybit linear mirror (``--days`` daily
``csv.gz`` files of ``--rows-per-day`` trades with UUID trade ids, in
the published layout), then runs ``lrvx.archives.bybit.range_to_lrvx``
over it once per worker count into a fresh tape and prints seconds and
rows per second for each. Every tape is checked against the first one
(record count, ``metadata.json``, returned counters).

``workers=1`` is the serial import. A worker count of ``auto`` omits
the argument, which leaves the importer's default (one per core). On a
build whose importer does not take ``workers``, the other rows say so
instead of failing.

    PYTHONPATH=build/python python3 benchmarks/archive_import_benchmark.py \\
        --days 8 --rows-per-day 200000 --workers 1,4,auto
"""
from __future__ import annotations

import argparse
import gzip
import json
import shutil
import sys
import tempfile
import time
import uuid
from datetime import date, timedelta
from pathlib import Path

_HEADER = ("timestamp,symbol,side,size,price,tickDirection,trdMatchID,"
           "grossValue,homeNotional,foreignNotional")
_DAY0 = date(2024, 1, 15)


def _write_day(dest: Path, day_index: int, rows: int) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    start = 1_705_276_800 + day_index * 86_400
    step = 86_400 / max(1, rows)
    with gzip.open(dest, "wt", encoding="utf-8", compresslevel=1) as f:
        f.write(_HEADER + "\n")
        for k in range(rows):
            qty = 0.001 * (k % 50 + 1)
            price = 42_000.0 + (k % 977) * 0.1
            ts = start + k * step
            trd = uuid.uuid5(uuid.NAMESPACE_OID, f"bench-{day_index}-{k}")
            f.write(f"{ts:.4f},BTCUSDT,{'Buy' if k % 2 else 'Sell'},{qty},"
                    f"{price:.1f},PlusTick,{trd},{qty * price * 1e8:.0f},"
                    f"{qty},{qty * price:.4f}\n")


def _tape_summary(tape: Path) -> tuple:
    import lrvx

    count = int(lrvx.DataReader(str(tape)).count())
    meta = json.loads((tape / "metadata.json").read_text())
    return count, meta


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--days", type=int, default=4)
    ap.add_argument("--rows-per-day", type=int, default=50_000)
    ap.add_argument("--workers", default="1,4,auto",
                    help="comma-separated worker counts; 'auto' omits the "
                         "argument (default: 1,4,auto)")
    ap.add_argument("--keep", default=None,
                    help="directory to build the mirror and tapes in, kept "
                         "afterwards (default: a temporary directory)")
    args = ap.parse_args(argv)

    from lrvx.archives import bybit

    root = Path(args.keep) if args.keep else Path(tempfile.mkdtemp(prefix="lrvx-archive-bench-"))
    mirror = root / "mirror"
    try:
        t0 = time.perf_counter()
        for i in range(args.days):
            day = _DAY0 + timedelta(days=i)
            dest = bybit._mirror_path(mirror, "BTCUSDT", "linear", day)
            if not dest.exists():
                _write_day(dest, i, args.rows_per_day)
        print(f"mirror: {args.days} day(s) x {args.rows_per_day} rows in "
              f"{time.perf_counter() - t0:.1f} s at {mirror}")

        last = (_DAY0 + timedelta(days=args.days - 1)).isoformat()
        total = args.days * args.rows_per_day
        reference = None
        print(f"{'workers':>8} {'seconds':>9} {'rows/s':>12}  check")
        for spec in [w.strip() for w in args.workers.split(",") if w.strip()]:
            kw = {} if spec == "auto" else {"workers": int(spec)}
            tape = root / f"tape-{spec}"
            shutil.rmtree(tape, ignore_errors=True)
            def run(**kw):
                return bybit.range_to_lrvx("BTCUSDT", "linear", _DAY0.isoformat(),
                                           last, tape, mirror=mirror, parallel=1, **kw)

            t0 = time.perf_counter()
            try:
                stats = run(**kw)
            except TypeError as exc:
                if "workers" not in str(exc):
                    raise
                if spec != "1":
                    print(f"{spec:>8} {'-':>9} {'-':>12}  workers not supported")
                    continue
                # An importer without the argument converts serially.
                shutil.rmtree(tape, ignore_errors=True)
                t0 = time.perf_counter()
                stats = run()
            dt = time.perf_counter() - t0
            summary = (_tape_summary(tape), dict(vars(stats)))
            if reference is None:
                reference = summary
                check = "reference"
            else:
                check = "same tape" if summary == reference else "DIFFERS"
            print(f"{spec:>8} {dt:9.2f} {total / dt:12,.0f}  {check}")
            if check == "DIFFERS":
                return 1
    finally:
        if not args.keep:
            shutil.rmtree(root, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
