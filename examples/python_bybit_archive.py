"""Bybit public archive round-trip — build a gzipped CSV in the layout
Bybit publishes for linear and inverse contracts, push it through
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


# timestamp, symbol, side, size, price, tickDirection, trdMatchID,
# grossValue, homeNotional, foreignNotional
_ROWS = [
    ("1700000000.5000", "BTCUSDT", "Buy",  0.01, 42_100.5, "ZeroPlusTick",
     "08ff9568-cb50-55d6-b497-13727eec09dc", 4.21005e+10, 0.01, 421.005),
    ("1700000001.2500", "BTCUSDT", "Sell", 0.02, 42_101.0, "ZeroMinusTick",
     "3f1d562a-57d4-5755-b305-2ade260deeaf", 8.4202e+10, 0.02, 842.02),
    ("1700000002.0000", "BTCUSDT", "Buy",  0.03, 42_100.7, "PlusTick",
     "f0039dc6-a46b-5c4d-b550-cad3be2fca6e", 1.263021e+11, 0.03, 1263.021),
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
