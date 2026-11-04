"""Bitget public archive round-trip — build a synthetic Bitget-format
CSV in memory, run `bitget.trades_to_lrvx`, then read the
resulting `.lrvx` back through `DataReader`.

CI-runnable companion to
[Import multi-exchange archives](../how-to/import-multi-exchange-archives.md).
No network — the fixture is built in memory.

Usage:
    cd /path/to/lrvx
    PYTHONPATH=build/python python3 docs/examples/python_bitget_archive.py
"""
from __future__ import annotations

import io
import shutil
import tempfile
import zipfile
from pathlib import Path

import lrvx
from lrvx.archives import bitget


_ROWS = [
    (100, 42_100.5, 0.01, "buy",  1_700_000_000_500),
    (101, 42_101.0, 0.02, "sell", 1_700_000_001_250),
    (102, 42_100.7, 0.03, "buy",  1_700_000_002_000),
]


def _build_zip(dest: Path) -> Path:
    buf = io.StringIO()
    buf.write("trade_id,price,size,side,timestamp_ms\n")
    for r in _ROWS:
        buf.write(",".join(str(x) for x in r) + "\n")
    with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(dest.with_suffix(".csv").name, buf.getvalue())
    return dest


def main() -> None:
    workdir = Path(tempfile.mkdtemp(prefix="lrvx-bitget-"))
    try:
        z = workdir / "BTCUSDT-trades-2024-01-15.zip"
        _build_zip(z)

        tape = workdir / "tape"
        stats = bitget.trades_to_lrvx(
            z, tape,
            symbol_id=1, symbol_name="BTCUSDT", market="umcbl",
        )
        trades = lrvx.DataReader(str(tape)).read_trades()
        print(
            f"converted: rows_read={stats.rows_read} "
            f"trades_written={stats.trades_written} "
            f"tape_trades={int(trades.size)}"
        )
        sides = [int(t["side"]) for t in trades]
        assert sides == [0, 1, 0], sides
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    main()
