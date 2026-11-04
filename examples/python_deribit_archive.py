"""Deribit public archive round-trip — build synthetic Deribit-format
CSVs in memory for a perpetual and an option-chain instrument, run
`deribit.trades_to_lrvx` on each, and read the resulting
`.lrvx` tapes back through `DataReader`.

CI-runnable companion to
[Import multi-exchange archives](../how-to/import-multi-exchange-archives.md).

Usage:
    cd /path/to/lrvx
    PYTHONPATH=build/python python3 docs/examples/python_deribit_archive.py
"""
from __future__ import annotations

import gzip
import io
import shutil
import tempfile
from pathlib import Path

import lrvx
from lrvx.archives import deribit


_ROWS_PERP = [
    (100, 1_700_000_000_500, "BTC-PERPETUAL", "buy",  42_100.5, 0.1, 42_100.0, 0.0, 42_098.0),
    (101, 1_700_000_001_250, "BTC-PERPETUAL", "sell", 42_101.0, 0.2, 42_101.0, 0.0, 42_099.5),
    (102, 1_700_000_002_000, "BTC-PERPETUAL", "buy",  42_100.7, 0.3, 42_100.7, 0.0, 42_098.5),
]

_ROWS_OPT = [
    (200, 1_700_000_000_000, "BTC-29MAR24-50000-C", "buy",  0.0500, 10.0, 0.0498, 0.55, 42_000.0),
    (201, 1_700_000_001_000, "BTC-29MAR24-50000-C", "sell", 0.0510, 5.0,  0.0511, 0.56, 42_010.0),
]


def _build_gz(dest: Path, rows) -> Path:
    buf = io.StringIO()
    buf.write("trade_id,timestamp_ms,instrument,side,price,amount,"
              "mark_price,iv,index_price\n")
    for r in rows:
        buf.write(",".join(str(x) for x in r) + "\n")
    with gzip.open(dest, "wt", encoding="utf-8") as f:
        f.write(buf.getvalue())
    return dest


def main() -> None:
    workdir = Path(tempfile.mkdtemp(prefix="lrvx-deribit-"))
    try:
        perp_gz = workdir / "BTC-PERPETUAL-2024-01-15.csv.gz"
        _build_gz(perp_gz, _ROWS_PERP)
        perp_tape = workdir / "tape-perp"
        ps = deribit.trades_to_lrvx(
            perp_gz, perp_tape,
            symbol_id=1, symbol_name="BTC-PERPETUAL", market="perpetual",
        )
        print(f"perp: rows_read={ps.rows_read} trades_written={ps.trades_written}")

        opt_gz = workdir / "BTC-29MAR24-50000-C-2024-01-15.csv.gz"
        _build_gz(opt_gz, _ROWS_OPT)
        opt_tape = workdir / "tape-option"
        os_ = deribit.trades_to_lrvx(
            opt_gz, opt_tape,
            symbol_id=1, symbol_name="BTC-29MAR24-50000-C", market="option",
        )
        print(f"option: rows_read={os_.rows_read} trades_written={os_.trades_written}")

        for label, tape in (("perp", perp_tape), ("option", opt_tape)):
            r = lrvx.DataReader(str(tape))
            trades = r.read_trades()
            print(f"  {label}: {int(trades.size)} trades in tape")
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    main()
