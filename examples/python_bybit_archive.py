"""Bybit public archive round-trip — build a synthetic gzipped CSV
matching Bybit's published column layout, push it through
`bybit.trades_to_lrvx`, and read the resulting `.lrvx` tape
back via `DataReader` to confirm the trade stream round-trips.

CI-runnable companion to
[Import multi-exchange archives](../how-to/import-multi-exchange-archives.md).
No network — the fixture is built in memory.

Usage:
    cd /path/to/lrvx
    PYTHONPATH=build/python python3 docs/examples/python_bybit_archive.py
"""
from __future__ import annotations

import gzip
import io
import shutil
import tempfile
from pathlib import Path

import lrvx
from lrvx.archives import bybit


_ROWS = [
    (1_700_000_000.500, "BTCUSDT", "Buy",  0.01, 42_100.5,
     "ZeroPlusTick",  "abc123", "", "", ""),
    (1_700_000_001.250, "BTCUSDT", "Sell", 0.02, 42_101.0,
     "ZeroMinusTick", "def456", "", "", ""),
    (1_700_000_002.000, "BTCUSDT", "Buy",  0.03, 42_100.7,
     "PlusTick",      "789012", "", "", ""),
]


def _build_gz(dest: Path) -> Path:
    buf = io.StringIO()
    buf.write("timestamp,symbol,side,size,price,tickDirection,"
              "trdMatchID,grossValue,homeNotional,foreignNotional\n")
    for r in _ROWS:
        buf.write(",".join(str(x) for x in r) + "\n")
    with gzip.open(dest, "wt", encoding="utf-8") as f:
        f.write(buf.getvalue())
    return dest


def main() -> None:
    workdir = Path(tempfile.mkdtemp(prefix="lrvx-bybit-"))
    try:
        gz_path = workdir / "BTCUSDT2024-01-15.csv.gz"
        _build_gz(gz_path)

        tape = workdir / "tape"
        stats = bybit.trades_to_lrvx(
            gz_path, tape,
            symbol_id=1, symbol_name="BTCUSDT", market="linear",
        )
        trades = lrvx.DataReader(str(tape)).read_trades()
        print(
            f"converted: rows_read={stats.rows_read} "
            f"trades_written={stats.trades_written} "
            f"tape_trades={int(trades.size)}"
        )
        # Buy → Side::BUY (0); Sell → Side::SELL (1).
        sides = [int(t["side"]) for t in trades]
        assert sides == [0, 1, 0], sides
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    main()
