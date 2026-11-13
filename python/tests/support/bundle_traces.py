"""A bundle with two round trips, and its expected trace, for the bundle
trace tests."""
from __future__ import annotations

import tarfile
from pathlib import Path

import lrvx
from lrvx import bundle, tape

TWO_ROUND_TRIPS = '''
import lrvx


class BuyThenSell(lrvx.Strategy):
    """Buys on the first trade it sees and sells on the third."""

    def __init__(self, symbols):
        super().__init__(symbols)
        self._seen = 0

    def on_trade(self, ctx, trade):
        self._seen += 1
        if self._seen == 1:
            self.market_buy(1.0)
        elif self._seen == 3:
            self.market_sell(1.0)
'''


def expected_trace(bundle_path: Path, work: Path) -> Path:
    out = work / "unpacked"
    with tarfile.open(bundle_path, "r") as tf:
        tf.extractall(out, filter="data")
    return out / "expected.lrvxrun"


def two_fill_trace(work: Path):
    """Packs a bundle whose strategy fills twice. Returns its path and the
    signals, order events and fills of its expected trace."""
    strat = work / "strategy.py"
    strat.write_text(TWO_ROUND_TRIPS)
    tape_dir = work / "tape"
    tape_dir.mkdir()
    registry = lrvx.SymbolRegistry()
    sym = int(registry.add_symbol("bundle", "BTCUSDT", tick_size=0.01))
    recorder = tape.make_recorder_hook(tape_dir)
    runner = lrvx.Runner(registry, on_signal=lambda _: None)
    runner.set_market_data_recorder(recorder)
    runner.start()
    for i in range(5):
        runner.on_trade(sym, 100.0 + i, 1.0, True, (i + 1) * 1_000_000_000)
    runner.stop()
    recorder.close()
    path = bundle.pack_bundle(strategy=strat, tape=tape_dir, output=work / "b.tar")
    reader = lrvx.TraceReader(str(expected_trace(path, work)))
    return path, reader.read_all_signals(), reader.read_all_order_events(), reader.read_all_fills()
