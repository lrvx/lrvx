"""A signal a pre-trade gate drops leaves its reason in lrvx.last_error().

The kill switch, the order validator and the risk manager each drop a signal
by returning False. The emit still returned an order id and nothing said the
order went nowhere. The reason is read on the thread that emitted, right
after the emit; every emit clears the last error first, so a reason left by
an earlier call is not read as this one's.
"""
from __future__ import annotations

import lrvx

T0 = 1_788_220_800_000_000_000


class Fake(lrvx.Executor):
    def __init__(self):
        super().__init__()
        self.submitted = []

    def submit(self, order):
        self.submitted.append(order.id)


class Deny:
    def __init__(self):
        self.kill = False
        self.validator = False
        self.risk = False


class Kill(lrvx.KillSwitch):
    def __init__(self, deny):
        super().__init__()
        self._deny = deny

    def check(self, signal):
        return not self._deny.kill


class Validator(lrvx.OrderValidator):
    def __init__(self, deny):
        super().__init__()
        self._deny = deny

    def validate(self, signal):
        return not self._deny.validator


class Risk(lrvx.RiskManager):
    def __init__(self, deny):
        super().__init__()
        self._deny = deny

    def allow(self, signal):
        return not self._deny.risk


class Scripted(lrvx.Strategy):
    def __init__(self, symbols, steps):
        super().__init__(symbols)
        self._steps = list(steps)
        self.ids = []
        self.errors = []

    def on_trade(self, ctx, trade):
        if not self._steps:
            return
        self.ids.append(self._steps.pop(0)(self))
        self.errors.append(lrvx.last_error())


def _run(steps, deny_before):
    """`deny_before[i]` names the gates that deny from step i on."""
    reg = lrvx.SymbolRegistry()
    sym = reg.add_symbol("test", "BTCUSDT", tick_size=0.01)
    delivered = []
    runner = lrvx.Runner(reg, delivered.append)
    executor = Fake()
    deny = Deny()
    gates = (Kill(deny), Validator(deny), Risk(deny))
    runner.set_executor(executor)
    runner.set_kill_switch(gates[0])
    runner.set_order_validator(gates[1])
    runner.set_risk_manager(gates[2])
    strat = Scripted([sym], steps)
    runner.add_strategy(strat)
    runner.start()
    for i in range(len(steps)):
        for name in deny_before.get(i, ()):
            setattr(deny, name, True)
        runner.on_trade(sym, 100.0, 1.0, True, T0 + i)
    runner.stop()
    return strat, executor, delivered


def _limit(s):
    return s.limit_sell(101.0, 1.0)


def test_each_gate_names_itself_and_the_order():
    strat, executor, delivered = _run(
        [_limit, _limit, _limit, _limit],
        {1: ("risk",), 2: ("validator",), 3: ("kill",)})

    assert strat.errors[0] == ""
    assert executor.submitted == [strat.ids[0]], "only the first order was sent"
    assert len(delivered) == 1

    for index, gate, token in ((1, "risk manager", "risk_manager"),
                               (2, "order validator", "order_validator"),
                               (3, "kill switch", "kill_switch")):
        why = strat.errors[index]
        assert why.endswith("[" + token + "]"), why
        assert gate in why and str(strat.ids[index]) in why and "limit" in why, why


def test_an_emit_that_passes_clears_an_earlier_reason():
    class Once(lrvx.KillSwitch):
        def __init__(self):
            super().__init__()
            self.calls = 0

        def check(self, signal):
            self.calls += 1
            return self.calls != 1

    reg = lrvx.SymbolRegistry()
    sym = reg.add_symbol("test", "BTCUSDT", tick_size=0.01)
    runner = lrvx.Runner(reg, lambda s: None)
    gate = Once()
    runner.set_kill_switch(gate)
    strat = Scripted([sym], [_limit, _limit, lambda s: s.market_buy(1.0),
                             lambda s: s.cancel_order(1)])
    runner.add_strategy(strat)
    runner.start()
    for i in range(4):
        runner.on_trade(sym, 100.0, 1.0, True, T0 + i)
    runner.stop()

    assert strat.errors[0].endswith("[kill_switch]")
    assert strat.errors[1:] == ["", "", ""], "each later emit cleared it"


def test_every_emit_method_clears_a_stale_reason(tmp_path):
    """The reason an unrelated call left must not be read after an emit."""
    missing = str(tmp_path / "no_such.lrvx")

    def stale(emit):
        def step(s):
            assert lrvx.recompress(missing, missing + ".out", "lz4") is False
            assert lrvx.last_error() != ""
            return emit(s)
        return step

    emits = [
        lambda s: s.market_buy(1.0),
        lambda s: s.market_sell(1.0),
        lambda s: s.limit_buy(99.0, 1.0),
        lambda s: s.limit_sell(101.0, 1.0),
        lambda s: s.stop_market(side="sell", trigger=95.0, qty=1.0),
        lambda s: s.stop_limit(side="sell", trigger=95.0, limit_price=94.0, qty=1.0),
        lambda s: s.take_profit_market(side="sell", trigger=105.0, qty=1.0),
        lambda s: s.take_profit_limit(side="sell", trigger=105.0, limit_price=106.0, qty=1.0),
        lambda s: s.trailing_stop(side="sell", offset=1.0, qty=1.0),
        lambda s: s.trailing_stop_percent(side="sell", callback_bps=50, qty=1.0),
        lambda s: s.close_position(),
        lambda s: s.cancel_order(12345),
        lambda s: s.cancel_all_orders(),
    ]
    strat, _, _ = _run([stale(e) for e in emits], {})
    assert strat.errors == [""] * len(emits)
