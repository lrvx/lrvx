"""What a bundle's expected trace holds, and what validate compares.

The runner's order ids are (subscriber id << 32) + n. Pack passed one to
write_signal, which took 32 bits, and swallowed the TypeError: every bundle
trace had fills and nothing else.
"""
from __future__ import annotations

import io
import json
import tarfile
import warnings
from pathlib import Path

import pytest

import lrvx
from lrvx import bundle, tape

_STRATEGY = '''
import lrvx


class FirstTradeBuy(lrvx.Strategy):
    def __init__(self, symbols):
        super().__init__(symbols)
        self._fired = False

    def on_trade(self, ctx, trade):
        if self._fired:
            return
        self._fired = True
        self.market_buy(1.0)
'''


def _inputs(work: Path):
    strat = work / "strategy.py"
    strat.write_text(_STRATEGY)
    tape_dir = work / "tape"
    tape_dir.mkdir()
    registry = lrvx.SymbolRegistry()
    sym = int(registry.add_symbol("bundle", "BTCUSDT", tick_size=0.01))
    recorder = tape.make_recorder_hook(tape_dir)
    runner = lrvx.Runner(registry, on_signal=lambda _: None)
    runner.set_market_data_recorder(recorder)
    runner.start()
    runner.on_trade(sym, 100.00, 1.0, True, 1_000_000_000)
    runner.on_trade(sym, 100.50, 0.5, False, 2_000_000_000)
    runner.on_trade(sym, 101.00, 1.5, True, 3_000_000_000)
    runner.stop()
    recorder.close()
    return strat, tape_dir


def _pack(work: Path) -> Path:
    strat, tape_dir = _inputs(work)
    return bundle.pack_bundle(strategy=strat, tape=tape_dir, output=work / "b.tar")


def _expected_trace(bundle_path: Path, work: Path) -> Path:
    out = work / "unpacked"
    with tarfile.open(bundle_path, "r") as tf:
        tf.extractall(out, filter="data")
    return out / "expected.lrvxrun"


def _validate(path: Path):
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", bundle.BundleTrustWarning)
        return bundle.validate_bundle(path)


def _as_packed_before(bundle_path: Path, work: Path, *, fills: int) -> Path:
    """The bundle with the trace an older pack wrote: fills only, and a
    manifest that does not say what its trace records."""
    old_trace = work / "old.lrvxrun"
    rec = lrvx.TraceRecorder(str(old_trace), strategy_id="strategy")
    for i in range(fills):
        rec.write_fill(run_ts_ns=1 + i, order_id=(7 << 32) + 1, fill_id=1,
                       price_raw=100, qty_raw=100, symbol_id=1)
    rec.close()

    out = work / "old.tar"
    with tarfile.open(bundle_path, "r") as src, tarfile.open(out, "w") as dst:
        for member in src.getmembers():
            if member.name.startswith("expected.lrvxrun"):
                continue
            data = src.extractfile(member).read() if member.isfile() else None
            if member.name == "manifest.json":
                manifest = json.loads(data)
                del manifest["lrvxrun_signals_recorded"]
                data = json.dumps(manifest, indent=2, sort_keys=True).encode()
                member.size = len(data)
            dst.addfile(member, io.BytesIO(data) if data is not None else None)
        dst.add(str(old_trace), arcname="expected.lrvxrun")
    return out


def test_a_packed_bundle_records_its_signal_and_order_event(tmp_path):
    trace = _expected_trace(_pack(tmp_path), tmp_path)
    reader = lrvx.TraceReader(str(trace))
    (sig,) = reader.read_all_signals()
    (order,) = reader.read_all_order_events()
    (fill,) = reader.read_all_fills()
    assert sig["signal_id64"] >= 1 << 32, "the runner's order id, whole"
    assert order["order_id"] == sig["signal_id64"]
    assert order["parent_signal_id"] == sig["signal_id64"]
    assert fill["order_id"] == sig["signal_id64"]


def test_the_manifest_says_the_trace_records_signals(tmp_path):
    path = _pack(tmp_path)
    with tarfile.open(path, "r") as tf:
        manifest = json.loads(tf.extractfile("manifest.json").read())
    assert manifest["lrvxrun_signals_recorded"] is True


def test_a_new_bundle_is_compared_on_all_three_counts(tmp_path):
    res = _validate(_pack(tmp_path))
    assert res.matches, res.diff
    assert res.expected["lrvxrun_compared"] == "signals, order events, fills"
    assert res.expected["lrvxrun_signal_count"] == 1
    assert res.expected["lrvxrun_order_count"] == 1
    assert res.actual["lrvxrun_signal_count"] == 1


def test_a_bundle_packed_before_still_validates_on_its_fills(tmp_path):
    old = _as_packed_before(_pack(tmp_path), tmp_path, fills=1)
    res = _validate(old)
    assert res.matches, res.diff
    assert res.expected["lrvxrun_compared"] == "fills"
    assert res.actual["lrvxrun_compared"] == "fills"
    assert "lrvxrun_signal_count" not in res.expected
    assert "lrvxrun_signal_count" not in res.actual
    assert res.expected["lrvxrun_fill_count"] == 1


def test_a_bundle_packed_before_still_fails_on_a_different_fill_count(tmp_path):
    old = _as_packed_before(_pack(tmp_path), tmp_path, fills=2)
    res = _validate(old)
    assert not res.matches
    assert any(d.startswith("lrvxrun_fill_count:") for d in res.diff), res.diff


def test_a_trace_write_failure_fails_the_pack(tmp_path, monkeypatch):
    strat, tape_dir = _inputs(tmp_path)

    def refuse(self, *args, **kwargs):
        raise OSError("disk full")

    monkeypatch.setattr(lrvx.TraceRecorder, "write_order_event", refuse)
    with pytest.raises(RuntimeError, match="disk full"):
        bundle.pack_bundle(strategy=strat, tape=tape_dir, output=tmp_path / "b.tar")
