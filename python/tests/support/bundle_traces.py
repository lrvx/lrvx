"""A bundle with two round trips, and its expected trace, for the bundle
trace tests; and the means to rewrite a bundle's trace, manifest or JSON
summary."""
from __future__ import annotations

import io
import json
import tarfile
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional

import lrvx
from lrvx import bundle, tape
from lrvx._lrvx import FillLiquidity, OrderEventKind

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


def read_trace(trace: Path) -> Dict[str, List[Dict[str, Any]]]:
    reader = lrvx.TraceReader(str(trace))
    return {
        "signals": reader.read_all_signals(),
        "orders": reader.read_all_order_events(),
        "fills": reader.read_all_fills(),
    }


def write_trace(trace: Path, recs: Dict[str, List[Dict[str, Any]]]) -> None:
    """Writes the records ``read_trace`` returns, every field as read."""
    rec = lrvx.TraceRecorder(str(trace), strategy_id="strategy")
    for s in recs["signals"]:
        rec.write_signal(run_ts_ns=s["run_ts_ns"], feed_ts_ns=s["feed_ts_ns"],
                         signal_id=s["signal_id64"], flags=s["flags"],
                         strength_raw=s["strength_raw"], name=s["name"],
                         symbol_ids=s["symbol_ids"], payload=s["payload"])
    for o in recs["orders"]:
        rec.write_order_event(run_ts_ns=o["run_ts_ns"], feed_ts_ns=o["feed_ts_ns"],
                              order_id=o["order_id"], parent_signal_id=o["parent_signal_id"],
                              price_raw=o["price_raw"], qty_raw=o["qty_raw"],
                              symbol_id=o["symbol_id"], event_kind=OrderEventKind(o["event_kind"]),
                              side=o["side"], order_type=o["order_type"], flags=o["flags"],
                              reason=o["reason"])
    for f in recs["fills"]:
        rec.write_fill(run_ts_ns=f["run_ts_ns"], feed_ts_ns=f["feed_ts_ns"],
                       order_id=f["order_id"], fill_id=f["fill_id"], price_raw=f["price_raw"],
                       qty_raw=f["qty_raw"], fee_raw=f["fee_raw"], symbol_id=f["symbol_id"],
                       side=f["side"], liquidity=FillLiquidity(f["liquidity"]))
    rec.close()


def repack(bundle_path: Path, out: Path, *, trace: Optional[Path] = None,
           manifest: Optional[Callable[[Dict[str, Any]], None]] = None,
           expected_output: Optional[Dict[str, Any]] = None) -> Path:
    """The bundle at ``bundle_path`` written to ``out`` with ``trace`` as its
    expected trace, its manifest edited by ``manifest``, and
    ``expected_output`` as its JSON summary; each only when given."""
    with tarfile.open(bundle_path, "r") as src, tarfile.open(out, "w") as dst:
        for member in src.getmembers():
            if trace is not None and member.name.startswith("expected.lrvxrun"):
                continue
            data = src.extractfile(member).read() if member.isfile() else None
            if member.name == "manifest.json" and manifest is not None:
                m = json.loads(data)
                manifest(m)
                data = json.dumps(m, indent=2, sort_keys=True).encode()
            elif member.name == "expected_output.json" and expected_output is not None:
                data = json.dumps(expected_output, indent=2, sort_keys=True).encode()
            if data is not None:
                member.size = len(data)
            dst.addfile(member, io.BytesIO(data) if data is not None else None)
        if trace is not None:
            dst.add(str(trace), arcname="expected.lrvxrun")
    return out


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
