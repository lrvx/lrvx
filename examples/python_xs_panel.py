"""Cross-sectional panel round-trip — write three per-symbol synthetic
tapes, run `build_close_panel` against them in each of the three
alignment modes, and print the (T, S) shape + NaN pattern so the
output matches what the how-to claims.

CI-runnable companion to
[Cross-sectional panel builder](../how-to/cross-sectional-panel.md).

Usage:
    cd /path/to/lrvx
    PYTHONPATH=build/python python3 docs/examples/python_xs_panel.py
"""
from __future__ import annotations

import shutil
import tempfile
from pathlib import Path

import numpy as np

import lrvx
from lrvx import panel as panel_mod


_BUCKET_NS = 60_000_000_000   # 1 minute


def _write_tape(root: Path, symbol: str, *,
                bucket_starts_ms: list[int],
                base_price: float) -> None:
    tape_dir = root / symbol
    tape_dir.mkdir(parents=True, exist_ok=True)
    w = lrvx.DataWriter(str(tape_dir), max_segment_mb=64,
                           exchange_id=0, compression="none")
    try:
        for i, ts_ms in enumerate(bucket_starts_ms):
            ts_ns = int(ts_ms) * 1_000_000 + 1_000_000
            w.write_trade(
                exchange_ts_ns=ts_ns, recv_ts_ns=ts_ns,
                price=float(base_price + i * 0.5),
                qty=1.0,
                trade_id=10_000 + i,
                symbol_id=1,
                side=0,
            )
    finally:
        w.close()


def main() -> None:
    workdir = Path(tempfile.mkdtemp(prefix="lrvx-xs-panel-"))
    try:
        base_ms = 1_700_000_000_000
        full = [base_ms + i * 60_000 for i in range(5)]
        sparse = [base_ms + i * 60_000 for i in (0, 1, 3, 4)]
        _write_tape(workdir, "BTCUSDT", bucket_starts_ms=full,   base_price=40_000.0)
        _write_tape(workdir, "ETHUSDT", bucket_starts_ms=full,   base_price=2_500.0)
        _write_tape(workdir, "SOLUSDT", bucket_starts_ms=sparse, base_price=150.0)

        symbols = ["BTCUSDT", "ETHUSDT", "SOLUSDT"]
        for mode in ("intersection", "union_nan", "union_ffill"):
            p = panel_mod.build_close_panel(
                symbols, bucket_ns=_BUCKET_NS,
                tape_root=workdir, align=mode,
            )
            nans = int(np.isnan(p.values).sum())
            print(f"mode={mode:13s} shape={p.values.shape} nans={nans}")

        # Returns panel: 2-bucket lookback.
        rp = panel_mod.build_returns_panel(
            symbols, bucket_ns=_BUCKET_NS,
            tape_root=workdir, lookback_n=2, align="intersection",
        )
        print(f"returns lookback_n={rp.lookback_n} shape={rp.values.shape}")
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    main()
