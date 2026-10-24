"""Binance public book archive round-trip — build small synthetic
bookTicker and bookDepth CSVs in the exact layout published on
``data.binance.vision``, push them through the converters, and read
back via ``DataReader.read_book_updates`` to confirm the book stream
round-trips.

CI-runnable companion to
[Import Binance book archives](../how-to/import-binance-book-archive.md).
No network — both fixtures are built in memory.

Usage:
    cd /path/to/lrvx
    PYTHONPATH=build/python python3 docs/examples/python_binance_book_archive.py
"""
from __future__ import annotations

import io
import shutil
import tempfile
import zipfile
from pathlib import Path

import lrvx
from lrvx.archives import binance


_T1_ROWS = [
    (100, 42_000.0, 1.0, 42_001.0, 1.0, 1_700_000_000_000, 1_700_000_000_000),
    (101, 42_000.0, 1.0, 42_001.0, 1.0, 1_700_000_001_000, 1_700_000_001_000),  # unchanged
    (102, 41_999.0, 2.0, 42_002.0, 1.5, 1_700_000_002_000, 1_700_000_002_000),
]


_D20_ROWS = [
    ("BTCUSDT", 1_700_000_000_000, 1, 10, "b", "snap", 42_000.0, 1.0),
    ("BTCUSDT", 1_700_000_000_000, 1, 10, "b", "snap", 41_999.0, 1.5),
    ("BTCUSDT", 1_700_000_000_000, 1, 10, "a", "snap", 42_001.0, 1.0),
    ("BTCUSDT", 1_700_000_000_000, 1, 10, "a", "snap", 42_002.0, 1.5),
    ("BTCUSDT", 1_700_000_001_000, 11, 11, "b", "set", 42_000.0, 0.0),
    ("BTCUSDT", 1_700_000_001_000, 11, 11, "b", "set", 42_000.5, 0.8),
    ("BTCUSDT", 1_700_000_001_000, 11, 11, "a", "set", 42_001.0, 0.5),
]


def _build_zip(dest: Path, rows) -> Path:
    buf = io.StringIO()
    for r in rows:
        buf.write(",".join(str(x) for x in r) + "\n")
    with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(dest.with_suffix(".csv").name, buf.getvalue())
    return dest


def main() -> None:
    workdir = Path(tempfile.mkdtemp(prefix="lrvx-binance-book-"))
    try:
        # bookTicker: 3 rows → 1 snapshot + 1 delta (one row unchanged).
        bt_zip = workdir / "BTCUSDT-bookTicker-2024-01-15.zip"
        _build_zip(bt_zip, _T1_ROWS)
        bt_tape = workdir / "tape-bt"
        bt_stats = binance.t1_to_lrvx(
            bt_zip, bt_tape, symbol_id=1, symbol_name="BTCUSDT",
            market="um-futures",
        )
        print(f"t1: snapshots={bt_stats.snapshots_written} "
              f"deltas={bt_stats.deltas_written} "
              f"skipped={bt_stats.rows_skipped}")

        # bookDepth: long-format snap → 1 snapshot, second group → 1 delta.
        d20_zip = workdir / "BTCUSDT-bookDepth-2024-01-15.zip"
        _build_zip(d20_zip, _D20_ROWS)
        d20_tape = workdir / "tape-d20"
        d20_stats = binance.depth20_to_lrvx(
            d20_zip, d20_tape, levels=20,
            symbol_id=1, symbol_name="BTCUSDT",
        )
        print(f"depth20: snapshots={d20_stats.snapshots_written} "
              f"deltas={d20_stats.deltas_written}")

        for label, tape in (("t1", bt_tape), ("depth20", d20_tape)):
            r = lrvx.DataReader(str(tape))
            headers, _ = r.read_book_updates()
            print(f"  {label}: {int(headers.size)} book events")
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    main()
