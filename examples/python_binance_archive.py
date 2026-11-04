"""Binance public aggTrades archive round-trip — build a tiny
synthetic zip in the exact layout published on data.binance.vision,
push it through ``lrvx.archives.binance.aggtrades_to_lrvx``,
then read the resulting ``.lrvx`` tape back via ``DataReader`` and
print a summary.

This example is the CI-runnable companion to
[Import the Binance public archive](../how-to/import-binance-archive.md).
It does not need network — the synthetic zip is built in-memory so
the test runs anywhere.

Usage:
    cd /path/to/lrvx
    PYTHONPATH=build/python python3 docs/examples/python_binance_archive.py
"""
from __future__ import annotations

import io
import shutil
import tempfile
import zipfile
from pathlib import Path

import lrvx
from lrvx.archives import binance


_ROWS = [
    # agg_id, price, qty, first_id, last_id, ts_ms, is_buyer_maker, is_best
    (1001, 42100.50, 0.005, 9001, 9001, 1_700_000_000_000, "True",  "True"),
    (1002, 42101.00, 0.010, 9002, 9003, 1_700_000_001_000, "False", "True"),
    (1003, 42100.75, 0.002, 9004, 9004, 1_700_000_002_500, "True",  "True"),
]


def _build_synthetic_zip(dest: Path) -> Path:
    buf = io.StringIO()
    for r in _ROWS:
        buf.write(",".join(str(x) for x in r) + "\n")
    with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("BTCUSDT-aggTrades-2024-01-15.csv", buf.getvalue())
    return dest


def main() -> None:
    workdir = Path(tempfile.mkdtemp(prefix="lrvx-binance-example-"))
    try:
        zip_path = workdir / "BTCUSDT-aggTrades-2024-01-15.zip"
        _build_synthetic_zip(zip_path)

        tape_dir = workdir / "tape"
        stats = binance.aggtrades_to_lrvx(
            zip_path,
            tape_dir,
            symbol_id=1,
            symbol_name="BTCUSDT",
            market="um-futures",
        )

        trades = lrvx.DataReader(str(tape_dir)).read_trades()
        print(
            f"converted: rows_read={stats.rows_read} "
            f"trades_written={stats.trades_written} "
            f"tape_trades={int(trades.size)}"
        )
        # is_buyer_maker=True maps to Side::SELL (1); False → SELL is False → BUY (0).
        expected_sides = [1 if r[6] == "True" else 0 for r in _ROWS]
        actual_sides = [int(t["side"]) for t in trades]
        assert actual_sides == expected_sides, (actual_sides, expected_sides)
        assert int(trades.size) == len(_ROWS)
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    main()
