"""OKX public archive round-trip — build an OKX trade file in the
layout OKX publishes (`instrument_name,trade_id,side,price,size,
created_time`) in memory, run `okx.trades_to_lrvx`, then read the
resulting `.lrvx` back through `DataReader` to confirm the trade
stream round-trips. Swap sizes are contracts; `qty_multiplier=0.01`
stores BTC-USDT-SWAP contracts (0.01 BTC each) in BTC.

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


# (instrument_name, trade_id, side, price, size, created_time)
_ROWS = [
    ("BTC-USDT-SWAP", 720073478, "buy",  42897.9,  8.0, 1_705_248_000_179),
    ("BTC-USDT-SWAP", 720073479, "sell", 42897.8,  1.0, 1_705_248_000_412),
    ("BTC-USDT-SWAP", 720073480, "buy",  42898.0, 25.0, 1_705_248_001_003),
]


def _build_zip(dest: Path) -> Path:
    buf = io.StringIO()
    buf.write("instrument_name,trade_id,side,price,size,created_time\n")
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
            qty_multiplier=0.01,
        )
        trades = lrvx.DataReader(str(tape)).read_trades()
        print(
            f"converted: rows_read={stats.rows_read} "
            f"trades_written={stats.trades_written} "
            f"tape_trades={int(trades.size)}"
        )
        sides = [int(t["side"]) for t in trades]
        assert sides == [0, 1, 0], sides
        btc = [int(t["qty_raw"]) / 1e8 for t in trades]
        assert btc == [0.08, 0.01, 0.25], btc
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    main()
