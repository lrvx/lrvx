"""Bitget public archive round-trip — build one part of a Bitget trade
day in the layout Bitget publishes (`trade_id,timestamp,price,side,
volume(quote),size(base)`) in memory, run `bitget.trades_to_lrvx`,
then read the resulting `.lrvx` back through `DataReader`.

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


# (trade_id, timestamp_ms, price, side, volume(quote), size(base))
_ROWS = [
    (1503354649689448450, 1_794_153_600_000, 75943.5, "buy",  7.59435,   0.0001),
    (1503354771215212544, 1_794_153_629_000, 75941.9, "sell", 25576.832, 0.3368),
    (1503354771215212546, 1_794_153_629_000, 75941.8, "buy",  721.44710, 0.0095),
]


def _build_zip(dest: Path) -> Path:
    buf = io.StringIO()
    buf.write("trade_id,timestamp,price,side,volume(quote),size(base)\n")
    for r in _ROWS:
        buf.write(",".join(str(x) for x in r) + "\n")
    with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(dest.with_suffix(".csv").name, buf.getvalue())
    return dest


def main() -> None:
    workdir = Path(tempfile.mkdtemp(prefix="lrvx-bitget-"))
    try:
        z = workdir / "BTCUSDT_UMCBL_20261109_001.zip"
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
