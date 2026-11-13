"""A bundle's trace records the trigger of each conditional order.

The submit record of a stop_market (and of every other conditional order)
carried price_raw 0 and no trigger anywhere, so two runs whose stops sat at
different triggers wrote the same trace and validate could not tell them
apart. The order event record has one price field, the order's limit price,
which the capture listener writes the same way; the trigger now goes in the
payload of the submit signal. A bundle whose manifest has
``conditional_triggers_recorded`` is packed and replayed that way; one
without it replays as it was packed, with no trigger.
"""
from __future__ import annotations

import json
import os
import sys
import warnings
from pathlib import Path

sys.path.insert(0, os.path.dirname(__file__))

import lrvx  # noqa: E402
from lrvx import bundle, tape  # noqa: E402
from lrvx._lrvx import OrderEventKind  # noqa: E402
from support.bundle_traces import expected_trace, read_trace, repack, write_trace  # noqa: E402

_KEY = "conditional_triggers_recorded"

_STRATEGY = '''
import lrvx


class Conditionals(lrvx.Strategy):
    """One of each conditional order on the first trade."""

    def __init__(self, symbols):
        super().__init__(symbols)
        self._seen = 0

    def on_trade(self, ctx, trade):
        self._seen += 1
        if self._seen == 1:
            self.stop_market("sell", 95.0, 1.0)
            self.stop_limit("sell", 94.0, 93.5, 1.0)
            self.take_profit_market("sell", 105.0, 1.0)
            self.take_profit_limit("sell", 106.0, 105.5, 1.0)
            self.trailing_stop("sell", 2.5, 1.0)
'''

_PRICES = [100.0, 99.0, 98.0, 97.0, 96.0]


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


def _validate(path: Path):
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", bundle.BundleTrustWarning)
        return bundle.validate_bundle(path)


def _pack(work: Path, source: str = _STRATEGY) -> Path:
    work.mkdir(parents=True, exist_ok=True)
    strat, tape_dir = _inputs(work, source)
    return bundle.pack_bundle(strategy=strat, tape=tape_dir, output=work / "b.tar")


def _payloads(recs):
    return [(s["name"], json.loads(s["payload"]) if s["payload"] else None)
            for s in recs["signals"]]


def test_each_conditional_submit_carries_its_trigger(tmp_path):
    recs = read_trace(expected_trace(_pack(tmp_path), tmp_path))
    assert _payloads(recs) == [
        ("stop_market", {"trigger_price_raw": 95 * 10**8}),
        ("stop_limit", {"trigger_price_raw": 94 * 10**8}),
        ("tp_market", {"trigger_price_raw": 105 * 10**8}),
        ("tp_limit", {"trigger_price_raw": 106 * 10**8}),
        ("trailing_stop", {"trailing_offset_raw": 250000000, "trailing_bps": 0}),
    ]
    submits = [o for o in recs["orders"]
               if OrderEventKind(o["event_kind"]) == OrderEventKind.SUBMIT]
    # price_raw stays the order's limit price, as the capture listener writes
    # it: none for the orders that fill at market.
    assert [o["price_raw"] for o in submits] == [0, 9350000000, 0, 10550000000, 0]


def test_a_plain_order_carries_no_payload(tmp_path):
    source = _STRATEGY.replace(
        'self.stop_market("sell", 95.0, 1.0)', 'self.limit_buy(90.0, 1.0)')
    recs = read_trace(expected_trace(_pack(tmp_path, source), tmp_path))
    assert _payloads(recs)[0] == ("limit", None)


def test_stops_at_other_triggers_no_longer_trace_the_same(tmp_path):
    a = read_trace(expected_trace(_pack(tmp_path / "a"), tmp_path / "a"))
    moved = _STRATEGY.replace('self.stop_market("sell", 95.0, 1.0)',
                              'self.stop_market("sell", 90.0, 1.0)')
    b = read_trace(expected_trace(_pack(tmp_path / "b", moved), tmp_path / "b"))
    assert a["orders"][0]["price_raw"] == b["orders"][0]["price_raw"] == 0
    assert a["signals"][0]["payload"] != b["signals"][0]["payload"]


def test_a_new_bundle_validates(tmp_path):
    res = _validate(_pack(tmp_path))
    assert res.matches, res.diff


def test_a_tampered_trigger_fails(tmp_path):
    path = _pack(tmp_path)
    recs = read_trace(expected_trace(path, tmp_path))
    recs["signals"][0]["payload"] = b'{"trigger_price_raw":9000000000}'
    trace = tmp_path / "edited.lrvxrun"
    write_trace(trace, recs)
    res = _validate(repack(path, tmp_path / "edited.tar", trace=trace))
    assert not res.matches
    assert any(d.startswith("lrvxrun_signals[0]") and "payload" in d for d in res.diff), res.diff


def test_a_bundle_without_the_marker_fails_on_the_missing_triggers(tmp_path):
    """Dropping the key from a new bundle's manifest replays with no
    triggers, which the recorded trace has."""
    path = _pack(tmp_path)
    res = _validate(repack(path, tmp_path / "edited.tar", manifest=lambda m: m.pop(_KEY)))
    assert not res.matches


def test_a_bundle_packed_before_still_validates(tmp_path):
    """A bundle packed when the trace held no trigger: no marker and a trace
    whose submit signals have no payload. Its replay records none either."""
    strat, tape_dir = _inputs(tmp_path)
    path = bundle.pack_bundle(strategy=strat, tape=tape_dir, output=tmp_path / "b.tar")
    old_trace = tmp_path / "old.lrvxrun"
    bundle._run_strategy_against_tape(strat, tape_dir, tick_size=None, lrvxrun_out=old_trace,
                                      record_triggers=False)
    assert all(s["payload"] == b"" for s in read_trace(old_trace)["signals"])
    old = repack(path, tmp_path / "old.tar", trace=old_trace, manifest=lambda m: m.pop(_KEY))
    res = _validate(old)
    assert res.matches, res.diff
    assert res.expected["lrvxrun_compared"] == "signal, order event and fill records"
