"""SimulatedExecutor.modify_order returns what the simulator decided.

It used to decide "taken" by looking at the order right after the replace.
With a replace ack latency the simulator holds the replacement until the ack
and the original keeps working meanwhile, so the order read then was the old
one and modify_order said False for a replacement that was about to go
through. It now returns "accepted", "pending" or "refused" from the event the
simulator emits for the request.
"""
from __future__ import annotations

import lrvx


def _sim() -> lrvx.SimulatedExecutor:
    sim = lrvx.SimulatedExecutor()
    sim.advance_clock(1_000_000_000)
    return sim


def test_without_latency_the_replacement_is_accepted():
    sim = _sim()
    sim.submit_order(1, "buy", 100.0, 1.0, type="limit", symbol=1)
    assert sim.modify_order(1, 99.0, 2.0, 1) == "accepted"
    assert sim.order_is_live(1)


def test_with_a_replace_ack_latency_the_replacement_is_pending():
    sim = _sim()
    sim.set_replace_ack_latency(5_000_000)
    sim.submit_order(1, "buy", 100.0, 1.0, type="limit", symbol=1)
    # The simulator took the request and holds it until the ack; the order
    # read right after the call is still the original, which is what made
    # the old answer False.
    assert sim.modify_order(1, 90.0, 2.0, 1) == "pending"
    assert sim.order_is_live(1)
    # Until the ack the original keeps working, at its own price and size.
    sim.on_bar(1, 95.0)
    fills = sim.fills_list()
    assert [(f["order_id"], f["price"], f["quantity"]) for f in fills] == [(1, 100.0, 1.0)]


def test_a_refused_replacement_says_so():
    sim = _sim()
    assert sim.modify_order(77, 99.0, 1.0, 1) == "refused"
    sim.set_replace_ack_latency(5_000_000)
    assert sim.modify_order(78, 99.0, 1.0, 1) == "refused"


def test_an_off_tick_replacement_is_refused():
    reg = lrvx.SymbolRegistry()
    sym = reg.add_symbol("test", "BTCUSDT", tick_size=0.5)
    sim = _sim()
    sim.set_symbol_registry(reg)
    sid = int(sym.id) if hasattr(sym, "id") else int(sym)
    sim.submit_order(1, "buy", 100.0, 1.0, type="limit", symbol=sid)
    assert sim.modify_order(1, 99.3, 1.0, sid) == "refused"
    assert sim.modify_order(1, 99.5, 1.0, sid) == "accepted"
