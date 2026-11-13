"""CcxtBroker against an in-process exchange: the position it finds at
start, the answer to an edit, and which ``on_order_update`` is the old form.

Three things were wrong. A position the account already held reached
``on_initial_position`` and nothing else, so the strategy read a position of
0 while the account was not flat. The answer to ``edit_order`` was dropped,
with whatever fill it carried. And the old four-argument ``on_order_update``
was recognised by counting every positional parameter, defaults included, so
``on_order_update(self, ctx, event, a=..., b=...)`` was taken for it.
"""
from __future__ import annotations

import asyncio
import os
import sys
import warnings

import pytest

sys.path.insert(0, os.path.dirname(__file__))

import lrvx  # noqa: E402
from lrvx.ccxt import CcxtBroker, _takes_legacy_order_update  # noqa: E402
from support.fake_ccxt import FakeOrderExchange  # noqa: E402


class Holding(lrvx.Strategy):
    """Writes down the position it reads on every trade, and what it is
    told of its orders."""

    def __init__(self, symbols, act=None):
        super().__init__(symbols)
        self.act = act
        self.order_id = None
        self.read = []
        self.initial = []
        self.fills = []
        self.updates = []

    def on_initial_position(self, sym, qty, avg):
        self.initial.append((sym, qty, avg))

    def on_trade(self, ctx, trade):
        self.read.append((ctx.position, self.position()))
        if self.act is not None and self.order_id is None:
            self.order_id = self.act(self)

    def on_fill(self, ctx, event):
        self.fills.append((event.order_id, event.status, event.fill_qty, event.fill_price,
                           ctx.position))

    def on_order_update(self, ctx, event):
        self.updates.append((event.order_id, event.status))


def _drive(fake, make_strategies, script=None, *, reconcile=True):
    errors = []
    broker = CcxtBroker("binance", exchange=fake,
                        on_error=lambda where, e: errors.append((where, repr(e))))
    out = {}

    async def go():
        btc = await broker.add_symbol("BTC/USDT")
        strategies = make_strategies([btc])
        for strategy in strategies:
            broker.add_strategy(strategy)
        out["strategies"] = strategies
        run = asyncio.create_task(broker.run(streams=("trades",), reconcile=reconcile))
        await asyncio.sleep(0.02)
        fake.feed_trades([{"price": 100.0, "amount": 1.0, "side": "buy",
                           "timestamp": 1_700_000_000_000}])
        await asyncio.sleep(0.02)
        if script is not None:
            await script(fake, strategies, broker)
            await asyncio.sleep(0.02)
        await broker.stop()
        try:
            await run
        except asyncio.CancelledError:
            pass

    asyncio.run(go())
    return out["strategies"], broker, errors


# ── the position found at start ──────────────────────────────────────


def test_a_position_found_at_start_is_what_the_strategy_reads():
    fake = FakeOrderExchange()
    fake.positions = [{"symbol": "BTC/USDT", "contracts": 2.0, "side": "short",
                       "entryPrice": 101.0}]
    (strategy,), _, errors = _drive(fake, lambda syms: [Holding(syms)])

    assert errors == []
    assert strategy.initial == [("BTC/USDT", 2.0, 101.0)], "the callback is still called"
    assert strategy.read == [(-2.0, -2.0)], "a short of 2, in the context and from position()"


def test_fills_add_to_the_position_found_at_start():
    fake = FakeOrderExchange()
    fake.positions = [{"symbol": "BTC/USDT", "contracts": 1.5, "side": "long",
                       "entryPrice": 99.0}]

    async def script(fake, strategies, broker):
        fake.fill("100", 0.5, 99.0)
        await asyncio.sleep(0.02)
        broker._dispatch_order_update(dict(fake.orders["100"]))

    (strategy,), _, errors = _drive(
        fake, lambda syms: [Holding(syms, lambda s: s.limit_buy(99.0, 0.5))], script)

    assert errors == []
    assert strategy.read[0] == (1.5, 1.5)
    assert [f[4] for f in strategy.fills] == [pytest.approx(2.0)]


def test_a_position_two_strategies_could_own_is_seeded_to_neither():
    fake = FakeOrderExchange()
    fake.positions = [{"symbol": "BTC/USDT", "contracts": 1.0, "side": "long",
                       "entryPrice": 99.0}]
    (first, second), _, errors = _drive(fake, lambda syms: [Holding(syms), Holding(syms)])

    assert first.read == [(0.0, 0.0)] and second.read == [(0.0, 0.0)]
    assert first.initial == second.initial == [("BTC/USDT", 1.0, 99.0)]
    assert len(errors) == 1 and errors[0][0] == "BTC/USDT"
    assert "2 strategies" in errors[0][1] and "seed_position" in errors[0][1]


def test_seed_position_refuses_a_strategy_of_another_runner_and_a_second_seed():
    registry = lrvx.SymbolRegistry()
    btc = registry.add_symbol("test", "BTC", 0.01)
    runner = lrvx.Runner(registry, lambda signal: None)
    mine, stranger = Holding([btc]), Holding([btc])
    runner.add_strategy(mine)
    runner.seed_position(mine, btc, -3.0, 100.0)
    assert mine.position() == -3.0
    with pytest.raises(ValueError, match="already holds"):
        runner.seed_position(mine, btc, 1.0, 100.0)
    with pytest.raises(ValueError, match="not attached"):
        runner.seed_position(stranger, btc, 1.0, 100.0)
    assert mine.position() == -3.0


# ── the answer to an edit ────────────────────────────────────────────


def test_a_fill_in_the_answer_to_an_edit_reaches_the_strategy():
    fake = FakeOrderExchange()
    fake.on_edit = {"fill": (0.4, 98.5)}

    async def script(fake, strategies, broker):
        strategies[0].modify_order(strategies[0].order_id, 98.5, 1.0)

    (strategy,), broker, errors = _drive(
        fake, lambda syms: [Holding(syms, lambda s: s.limit_buy(99.0, 1.0))], script,
        reconcile=False)

    assert errors == []
    assert [c[0] for c in fake.calls] == ["create", "edit"]
    oid = strategy.order_id
    assert strategy.fills == [(oid, "PARTIALLY_FILLED", pytest.approx(0.4),
                               pytest.approx(98.5), pytest.approx(0.4))]
    assert broker._lrvx_to_ccxt_order == {oid: "100"}, "still working"


def test_an_order_finished_in_the_answer_to_an_edit_is_closed():
    fake = FakeOrderExchange()
    fake.on_edit = {"fill": (1.0, 98.5)}

    async def script(fake, strategies, broker):
        strategies[0].modify_order(strategies[0].order_id, 98.5, 1.0)

    (strategy,), broker, errors = _drive(
        fake, lambda syms: [Holding(syms, lambda s: s.limit_buy(99.0, 1.0))], script,
        reconcile=False)

    assert errors == []
    assert [f[1] for f in strategy.fills] == ["FILLED"]
    assert broker._lrvx_to_ccxt_order == {} and broker._placed_orders == {}
    assert broker._order_progress == {} and broker._ccxt_orders_by_sym == {}


def test_an_edit_answered_under_a_new_id_reports_that_orders_fill():
    fake = FakeOrderExchange()
    fake.on_edit = {"replace": True, "fill": (0.25, 98.0)}

    async def script(fake, strategies, broker):
        strategies[0].modify_order(strategies[0].order_id, 98.0, 1.0)

    (strategy,), broker, errors = _drive(
        fake, lambda syms: [Holding(syms, lambda s: s.limit_buy(99.0, 1.0))], script,
        reconcile=False)

    assert errors == []
    oid = strategy.order_id
    assert [(f[0], f[1], f[2]) for f in strategy.fills] == [
        (oid, "PARTIALLY_FILLED", pytest.approx(0.25))]
    assert broker._lrvx_to_ccxt_order == {oid: "101"}


# ── which on_order_update is the old form ────────────────────────────


def _strategy_with(fn):
    return type("S", (), {"on_order_update": fn})()


def test_the_old_form_is_four_required_arguments():
    def old(self, ccxt_sym, status, filled, avg_price):
        pass

    def old_with_an_extra_default(self, ccxt_sym, status, filled, avg_price, note=None):
        pass

    assert _takes_legacy_order_update(_strategy_with(old))
    assert _takes_legacy_order_update(_strategy_with(old_with_an_extra_default))


def test_defaults_do_not_make_the_new_form_the_old_one():
    def new(self, ctx, event):
        pass

    def new_with_two_defaults(self, ctx, event, a=1, b=2):
        pass

    def new_with_keyword_only(self, ctx, event, *, a, b):
        pass

    def anything(self, *args, **kwargs):
        pass

    def new_then_anything(self, ctx, event, *rest):
        pass

    for fn in (new, new_with_two_defaults, new_with_keyword_only, anything, new_then_anything):
        assert not _takes_legacy_order_update(_strategy_with(fn)), fn.__name__


def test_the_bound_callable_is_what_is_judged():
    def old(self, ccxt_sym, status, filled, avg_price):
        pass

    def four(ccxt_sym, status, filled, avg_price):
        pass

    # Takes four once bound, whatever it is made of.
    assert _takes_legacy_order_update(_strategy_with(staticmethod(four)))
    assert _takes_legacy_order_update(_strategy_with(classmethod(old)))
    # A static method written with a self it never gets requires five.
    assert not _takes_legacy_order_update(_strategy_with(staticmethod(old)))
    # An attribute set on the instance.
    plain = type("S", (), {})()
    plain.on_order_update = four
    assert _takes_legacy_order_update(plain)


def test_no_method_and_the_base_class_method_are_not_the_old_form():
    assert not _takes_legacy_order_update(object())
    assert not _takes_legacy_order_update(_strategy_with(None))
    assert not _takes_legacy_order_update(lrvx.Strategy([]))

    class Plain(lrvx.Strategy):
        pass

    assert not _takes_legacy_order_update(Plain([]))


def test_a_new_form_override_with_defaults_hears_its_order_events():
    """It was wrapped as the old form: (ctx, event) calls were dropped."""
    fake = FakeOrderExchange()

    class WithDefaults(Holding):
        def on_order_update(self, ctx, event, a=1, b=2):
            self.updates.append((event.order_id, event.status))

    async def script(fake, strategies, broker):
        broker._dispatch_order_update(dict(fake.orders["100"]))

    with warnings.catch_warnings():
        warnings.simplefilter("error", DeprecationWarning)
        (strategy,), _, errors = _drive(
            fake, lambda syms: [WithDefaults(syms, lambda s: s.limit_buy(99.0, 1.0))], script,
            reconcile=False)

    assert errors == []
    assert strategy.updates == [(strategy.order_id, "ACCEPTED")]
