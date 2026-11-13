"""A side or time-in-force the strategy API does not know is refused.

``limit_buy(..., tif="iok")`` placed a GTC order and
``stop_market(side="byu", ...)`` a sell: anything the conversion did not
recognise fell through to a default, and a typo sent an order nobody asked
for. Both raise ValueError naming the value; nothing is emitted.
"""
from __future__ import annotations

import pytest

import lrvx

T0 = 1_788_220_800_000_000_000


def _run(step):
    reg = lrvx.SymbolRegistry()
    sym = reg.add_symbol("test", "BTCUSDT", tick_size=0.01)
    signals = []
    runner = lrvx.Runner(reg, signals.append)
    raised = []

    class Once(lrvx.Strategy):
        def on_trade(self, ctx, trade):
            try:
                step(self)
            except ValueError as e:
                raised.append(str(e))

    runner.add_strategy(Once([sym]))
    runner.start()
    runner.on_trade(sym, 100.0, 1.0, True, T0)
    runner.stop()
    return signals, raised


@pytest.mark.parametrize("emit", [
    lambda s, tif: s.limit_buy(99.0, 1.0, tif=tif),
    lambda s, tif: s.limit_sell(101.0, 1.0, tif=tif),
], ids=["limit_buy", "limit_sell"])
def test_an_unknown_tif_raises_and_emits_nothing(emit):
    signals, raised = _run(lambda s: emit(s, "iok"))
    assert signals == []
    assert len(raised) == 1 and "'iok'" in raised[0] and "post_only" in raised[0], raised

    # Good-till-date needs an expiry these methods do not take.
    signals, raised = _run(lambda s: emit(s, "gtd"))
    assert signals == [] and len(raised) == 1 and "expiry" in raised[0], raised

    for tif in ("gtc", "ioc", "fok", "post_only"):
        signals, raised = _run(lambda s, tif=tif: emit(s, tif))
        assert raised == [] and len(signals) == 1, tif


@pytest.mark.parametrize("emit", [
    lambda s, side: s.stop_market(side=side, trigger=95.0, qty=1.0),
    lambda s, side: s.stop_limit(side=side, trigger=95.0, limit_price=94.0, qty=1.0),
    lambda s, side: s.take_profit_market(side=side, trigger=105.0, qty=1.0),
    lambda s, side: s.take_profit_limit(side=side, trigger=105.0, limit_price=106.0, qty=1.0),
    lambda s, side: s.trailing_stop(side=side, offset=1.0, qty=1.0),
    lambda s, side: s.trailing_stop_percent(side=side, callback_bps=50, qty=1.0),
], ids=["stop_market", "stop_limit", "take_profit_market", "take_profit_limit",
        "trailing_stop", "trailing_stop_percent"])
def test_an_unknown_side_raises_and_emits_nothing(emit):
    signals, raised = _run(lambda s: emit(s, "byu"))
    assert signals == []
    assert len(raised) == 1 and "'byu'" in raised[0], raised

    signals, raised = _run(lambda s: emit(s, "buy"))
    assert raised == [] and [sig.side for sig in signals] == ["buy"]
    signals, raised = _run(lambda s: emit(s, "sell"))
    assert raised == [] and [sig.side for sig in signals] == ["sell"]
