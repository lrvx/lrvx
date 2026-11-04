"""OKX public archive round-trip — build a synthetic OKX-format CSV
in memory, run `okx.trades_to_lrvx`, then read the resulting
`.lrvx` back through `DataReader` to confirm the trade stream
round-trips.

CI-runnable companion to
[Import multi-exchange archives](../how-to/import-multi-exchange-archives.md).
No network — the fixture is built in memory.

Usage:
    cd /path/to/lrvx
    PYTHONPATH=build/python python3 docs/examples/python_okx_archive.py
"""
from __future__ import annotations

import io
import shutil
import tempfile
import zipfile
from pathlib import Path

import lrvx
from lrvx.archives import okx


_ROWS = [
    (100, "buy",  0.01, 42_100.5, 1_700_000_000_500),
    (101, "sell", 0.02, 42_101.0, 1_700_000_001_250),
    (102, "buy",  0.03, 42_100.7, 1_700_000_002_000),
]


def _build_zip(dest: Path) -> Path:
    buf = io.StringIO()
    buf.write("trade_id,side,size,price,timestamp_ms\n")
    for r in _ROWS:
        buf.write(",".join(str(x) for x in r) + "\n")
    with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(dest.with_suffix(".csv").name, buf.getvalue())
    return dest


def main() -> None:
    workdir = Path(tempfile.mkdtemp(prefix="lrvx-okx-"))
    try:
        z = workdir / "BTC-USDT-SWAP-trades-2024-01-15.zip"
        _build_zip(z)

        tape = workdir / "tape"
        stats = okx.trades_to_lrvx(
            z, tape,
            symbol_id=1, symbol_name="BTC-USDT-SWAP", market="swap",
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
