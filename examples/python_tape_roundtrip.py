"""
Tape recording round-trip — record synthetic trades via the recorder
hook, then replay them back through ``replay_tape`` and confirm the
events round-trip exactly.

This example is the CI-runnable companion to
[Record and replay market data](../how-to/tape-record.md). It does not
need ccxt or a live exchange — synthetic trades are pumped directly
through ``runner.on_trade`` so the recorder writes without a network
dependency.

Usage:
    cd /path/to/lrvx
    PYTHONPATH=build/python python3 docs/examples/python_tape_roundtrip.py
"""
from __future__ import annotations

import shutil
import tempfile
from pathlib import Path

import lrvx
from lrvx.tape import inspect_tape, make_recorder_hook, replay_tape


def main() -> None:
    out = Path(tempfile.mkdtemp(prefix="lrvx-tape-example-"))
    try:
        registry = lrvx.SymbolRegistry()
        sym = registry.add_symbol("example", "BTCUSDT", tick_size=0.01)

        recorder = make_recorder_hook(out, max_segment_mb=64)
        runner = lrvx.Runner(registry, on_signal=lambda _sig: None)
        runner.set_market_data_recorder(recorder)
        runner.start()

        synthetic = [
            (101.50, 0.5, True,  1_000_000_000),
            (101.55, 0.3, False, 1_500_000_000),
            (101.60, 1.2, True,  2_000_000_000),
        ]
        for price, qty, is_buy, ts_ns in synthetic:
            runner.on_trade(sym, price, qty, is_buy, ts_ns)

        runner.stop()
        recorder.close()

        stats = inspect_tape(out)
        print(
            f"recorded: trades={stats.trade_count} "
            f"first_ts={stats.first_ts_ns} last_ts={stats.last_ts_ns}"
        )
        assert stats.trade_count == 3
        assert stats.first_ts_ns == 1_000_000_000
        assert stats.last_ts_ns == 2_000_000_000

        seen: list[tuple[int, int, float, float, int]] = []
        n = replay_tape(
            out,
            on_trade=lambda ts, s, p, q, side: seen.append((ts, s, p, q, side)),
        )
        print(f"replayed {n} trades")
        assert n == 3
        assert [row[2] for row in seen] == [101.50, 101.55, 101.60]
        assert [row[3] for row in seen] == [0.5, 0.3, 1.2]
        # side: 0 = buy, 1 = sell — matches our (True, False, True) input.
        assert [row[4] for row in seen] == [0, 1, 0]

        print("round-trip OK")
    finally:
        shutil.rmtree(out, ignore_errors=True)


if __name__ == "__main__":
    main()
