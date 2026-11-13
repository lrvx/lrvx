"""A bundle reproduces a strategy's cancels and modifies.

Pack routed only the order-placing signals to the simulator: a cancel, a
cancel-all or a modify was dropped, so a cancelled order still filled, a
modified one filled as first placed, and validate replayed the same mistake
and reported OK. A bundle whose manifest has ``cancels_and_modifies_routed``
now sends them to the simulator as the backtest runner does and records them;
one without it replays as it was packed.
"""
from __future__ import annotations

import importlib.util
import json
import os
import sys
import tarfile
import warnings
from pathlib import Path

import pytest

sys.path.insert(0, os.path.dirname(__file__))

import lrvx  # noqa: E402
from lrvx import bundle, tape  # noqa: E402
from lrvx._lrvx import OrderEventKind  # noqa: E402
from support.bundle_traces import expected_trace, read_trace, repack, write_trace  # noqa: E402

_STRATEGY = '''
import lrvx


class PlaceModifyCancel(lrvx.Strategy):
    """Two sell stops on the first trade; the one at 95 is moved to 92 for
    twice the size, the one at 96 is cancelled. A third stop is placed on the
    sixth trade and taken off by a cancel-all on the seventh."""

    def __init__(self, symbols):
        super().__init__(symbols)
        self._seen = 0
        self._moved = 0
        self._cancelled = 0

    def on_trade(self, ctx, trade):
        self._seen += 1
        if self._seen == 1:
            self._moved = self.stop_market("sell", 95.0, 1.0)
            self._cancelled = self.stop_market("sell", 96.0, 1.0)
        elif self._seen == 2:
            self.modify_order(self._moved, 92.0, 2.0)
        elif self._seen == 3:
            self.cancel_order(self._cancelled)
        elif self._seen == 6:
            self.stop_market("sell", 85.0, 1.0)
        elif self._seen == 7:
            self.cancel_all_orders()
'''

# Every stop would fire on this path if nothing were cancelled or moved: the
# one at 96 and the one at 95 on the trade at 95, the third on the trade at 80.
_PRICES = (100.0, 100.0, 100.0, 95.0, 91.0, 91.0, 91.0, 80.0)
_ROUTED = "cancels_and_modifies_routed"


_OFF_TICK_MODIFY = '''
import lrvx


class PlaceModifyCancel(lrvx.Strategy):
    """A sell stop at 95, then a modify to a trigger off the 0.5 tick."""

    def __init__(self, symbols):
        super().__init__(symbols)
        self._seen = 0
        self._id = 0

    def on_trade(self, ctx, trade):
        self._seen += 1
        if self._seen == 1:
            self._id = self.stop_market("sell", 95.0, 1.0)
        elif self._seen == 2:
            self.modify_order(self._id, 92.25, 2.0)
'''


def _inputs(work: Path, source: str = _STRATEGY):
    strat = work / "strategy.py"
    strat.write_text(source)
    tape_dir = work / "tape"
    tape_dir.mkdir()
    registry = lrvx.SymbolRegistry()
    sym = int(registry.add_symbol("bundle", "BTCUSDT", tick_size=0.01))
    recorder = tape.make_recorder_hook(tape_dir)
    runner = lrvx.Runner(registry, on_signal=lambda _: None)
    runner.set_market_data_recorder(recorder)
    runner.start()
    for i, price in enumerate(_PRICES):
        runner.on_trade(sym, price, 1.0, False, (i + 1) * 1_000_000_000)
    runner.stop()
    recorder.close()
    return strat, tape_dir


def _backtest_fills(strat: Path, tape_dir: Path, tick_size: float = 0.01):
    """What the backtest runner fills when it runs the same strategy on the
    same tape: the reference the bundle has to reproduce."""
    spec = importlib.util.spec_from_file_location("bundle_cancel_modify_ref", strat)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)

    class Recording(mod.PlaceModifyCancel):
        def __init__(self, symbols):
            super().__init__(symbols)
            self.fills = []

        def on_fill(self, ctx, ev):
            self.fills.append({"side": ev.side, "price": ev.fill_price,
                               "quantity": ev.fill_qty})

    registry = lrvx.SymbolRegistry()
    sym = registry.add_symbol("bundle", "BTCUSDT", tick_size=tick_size)
    bt = lrvx.BacktestRunner(registry, fee_rate=0.0, initial_capital=10_000)
    s = Recording([sym])
    bt.set_strategy(s)
    bt.run_tape(str(tape_dir))
    return s.fills


def _pack(work: Path) -> Path:
    strat, tape_dir = _inputs(work)
    return bundle.pack_bundle(strategy=strat, tape=tape_dir, output=work / "b.tar")


def _member(path: Path, name: str):
    with tarfile.open(path, "r") as tf:
        return json.loads(tf.extractfile(name).read())


def _validate(path: Path):
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", bundle.BundleTrustWarning)
        return bundle.validate_bundle(path)


def _with_trace(path: Path, work: Path, edit) -> Path:
    recs = read_trace(expected_trace(path, work))
    edit(recs)
    trace = work / "edited.lrvxrun"
    write_trace(trace, recs)
    return repack(path, work / "edited.tar", trace=trace)


def test_the_reference_backtest_cancels_and_modifies(tmp_path):
    strat, tape_dir = _inputs(tmp_path)
    assert _backtest_fills(strat, tape_dir) == [
        {"side": "sell", "price": 91.0, "quantity": 2.0}]


def test_pack_fills_what_the_backtest_runner_fills(tmp_path):
    strat, tape_dir = _inputs(tmp_path)
    path = bundle.pack_bundle(strategy=strat, tape=tape_dir, output=tmp_path / "b.tar")
    expected = _member(path, "expected_output.json")
    fills = [{k: f[k] for k in ("side", "price", "quantity")} for f in expected["fills"]]
    assert fills == _backtest_fills(strat, tape_dir)
    assert expected["cancel_count"] == 2
    assert expected["modify_count"] == 1
    assert _member(path, "manifest.json")[_ROUTED] is True


def test_the_trace_records_the_cancels_and_the_modify(tmp_path):
    path = _pack(tmp_path)
    recs = read_trace(expected_trace(path, tmp_path))
    assert [s["name"] for s in recs["signals"]] == [
        "stop_market", "stop_market", "modify", "cancel", "stop_market", "cancel_all"]
    moved, cancelled = (s["signal_id64"] for s in recs["signals"][:2])
    third = recs["signals"][4]["signal_id64"]
    events = [(OrderEventKind(o["event_kind"]), o["order_id"]) for o in recs["orders"]]
    assert events == [
        (OrderEventKind.SUBMIT, moved), (OrderEventKind.SUBMIT, cancelled),
        (OrderEventKind.MODIFY, moved), (OrderEventKind.CANCEL, cancelled),
        (OrderEventKind.SUBMIT, third), (OrderEventKind.CANCEL, third)]
    modify = recs["orders"][2]
    assert (modify["price_raw"], modify["qty_raw"]) == (92 * 10**8, 2 * 10**8)
    assert recs["signals"][2]["signal_id64"] == moved
    assert recs["signals"][3]["signal_id64"] == cancelled


def test_a_refused_modify_leaves_the_order_and_writes_no_modify_event(tmp_path):
    strat, tape_dir = _inputs(tmp_path, _OFF_TICK_MODIFY)
    path = bundle.pack_bundle(strategy=strat, tape=tape_dir, output=tmp_path / "b.tar",
                              tick_size=0.5)
    expected = _member(path, "expected_output.json")
    fills = [{k: f[k] for k in ("side", "price", "quantity")} for f in expected["fills"]]
    assert fills == _backtest_fills(strat, tape_dir, tick_size=0.5) == [
        {"side": "sell", "price": 95.0, "quantity": 1.0}]
    assert expected["modify_count"] == 0
    recs = read_trace(expected_trace(path, tmp_path))
    assert [s["name"] for s in recs["signals"]] == ["stop_market", "modify"]
    assert [OrderEventKind(o["event_kind"]) for o in recs["orders"]] == [OrderEventKind.SUBMIT]
    res = _validate(path)
    assert res.matches, res.diff


def test_a_new_bundle_validates_record_by_record(tmp_path):
    res = _validate(_pack(tmp_path))
    assert res.matches, res.diff
    assert res.expected["lrvxrun_compared"] == "signal, order event and fill records"
    assert res.actual["modify_count"] == 1


@pytest.mark.parametrize("edit", [
    pytest.param(lambda r: r["orders"].pop(3), id="cancel-event-missing"),
    pytest.param(lambda r: r["orders"].pop(5), id="cancel-all-event-missing"),
    pytest.param(lambda r: r["orders"][2].update(qty_raw=10**8), id="modify-quantity"),
    pytest.param(lambda r: r["orders"][2].update(price_raw=95 * 10**8), id="modify-price"),
    pytest.param(lambda r: r["signals"].pop(2), id="modify-signal-missing"),
])
def test_a_tampered_cancel_or_modify_record_fails(tmp_path, edit):
    res = _validate(_with_trace(_pack(tmp_path), tmp_path, edit))
    assert not res.matches
    assert any(d.startswith(("lrvxrun_order_events[", "lrvxrun_signals["))
               for d in res.diff), res.diff


def test_a_tampered_cancel_count_fails(tmp_path):
    path = _pack(tmp_path)
    expected = _member(path, "expected_output.json")
    expected["cancel_count"] = 1
    res = _validate(repack(path, tmp_path / "edited.tar", expected_output=expected))
    assert not res.matches
    assert "cancel_count: actual=2 expected=1" in res.diff, res.diff


def test_a_bundle_without_the_marker_fails_on_what_routing_changed(tmp_path):
    """Dropping the key from a new bundle's manifest replays with the
    actions dropped, which the recorded fills and trace do not match."""
    path = _pack(tmp_path)
    res = _validate(repack(path, tmp_path / "edited.tar",
                           manifest=lambda m: m.pop(_ROUTED)))
    assert not res.matches


def test_a_bundle_packed_before_still_validates(tmp_path):
    """A bundle packed when cancels and modifies were dropped: no marker, a
    JSON summary and a trace without them. Its replay drops them again and
    it validates on its records as before."""
    strat, tape_dir = _inputs(tmp_path)
    path = bundle.pack_bundle(strategy=strat, tape=tape_dir, output=tmp_path / "b.tar")
    old_trace = tmp_path / "old.lrvxrun"
    old_output = bundle._run_strategy_against_tape(
        strat, tape_dir, tick_size=None, lrvxrun_out=old_trace,
        route_cancels_and_modifies=False)
    assert "cancel_count" not in old_output
    assert old_output["fill_count"] == 3, "every stop fired: nothing was cancelled"
    old = repack(path, tmp_path / "old.tar", trace=old_trace, expected_output=old_output,
                 manifest=lambda m: m.pop(_ROUTED))

    res = _validate(old)
    assert res.matches, res.diff
    assert res.expected["lrvxrun_compared"] == "signal, order event and fill records"
    assert "cancel_count" not in res.actual
    assert [s["name"] for s in res.actual["lrvxrun_signals"]] == ["stop_market"] * 3
