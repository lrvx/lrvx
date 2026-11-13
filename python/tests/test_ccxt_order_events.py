"""Order updates and fills on CcxtBroker, against an in-process exchange.

The broker called ``strategy.on_order_update(ccxt_sym, status, filled,
avg)``. ``lrvx.Strategy.on_order_update`` takes ``(ctx, event)``, so a
strategy that did not override it raised TypeError on every update, and one
written for a backtest received arguments it could not read. Updates and
fills now arrive as they do in a backtest and in paper mode: fills in
``on_fill(ctx, event)`` and nowhere else, everything else in
``on_order_update(ctx, event)``.

The orders stream is opened by the broker itself before it places an order,
and a cancel-all tells the runner which orders it finished.
"""
from __future__ import annotations

import asyncio
import os
import sys
import warnings

import numpy as np
import pytest

sys.path.insert(0, os.path.dirname(__file__))

import lrvx  # noqa: E402
from lrvx.ccxt import CcxtBroker  # noqa: E402
from lrvx.paper import PaperBroker  # noqa: E402
from support.fake_ccxt import FakeOrderExchange  # noqa: E402

T0 = 1_788_220_800_000_000_000


class Recording(lrvx.Strategy):
    """Buys 1 at market on its first trade and writes down what it hears."""

    def __init__(self, symbols, act=None):
        super().__init__(symbols)
        self.act = act or (lambda s: s.market_buy(1.0))
        self.order_id = None
        self.fills = []
        self.updates = []

    def on_trade(self, ctx, trade):
        if self.order_id is None:
            self.order_id = self.act(self)

    def on_fill(self, ctx, event):
        self.fills.append((event.order_id, event.status, event.side, event.fill_qty,
                           event.fill_price, ctx.position))

    def on_order_update(self, ctx, event):
        self.updates.append((event.order_id, event.status))


def _drive(fake, make_strategy, script, *, streams=("trades",)):
    """Run a broker over `fake`; `script(fake, strategy, broker)` is an
    async function run once the strategy has seen one trade."""
    errors = []
    broker = CcxtBroker("binance", exchange=fake,
                        on_error=lambda where, e: errors.append((where, repr(e))))
    out = {}

    async def go():
        btc = await broker.add_symbol("BTC/USDT")
        strategy = make_strategy([btc])
        broker.add_strategy(strategy)
        out["strategy"] = strategy
        run = asyncio.create_task(broker.run(streams=streams, reconcile=False))
        await asyncio.sleep(0)
        fake.feed_trades([{"price": 100.0, "amount": 1.0, "side": "buy",
                           "timestamp": 1_700_000_000_000}])
        await asyncio.sleep(0.02)
        await script(fake, strategy, broker)
        await asyncio.sleep(0.02)
        await broker.stop()
        try:
            await run
        except asyncio.CancelledError:
            pass

    asyncio.run(go())
    return out["strategy"], broker, errors


async def _nothing(fake, strategy, broker):
    pass


def test_an_unoverridden_strategy_takes_an_order_update():
    """The base class's on_order_update(ctx, event) was called with four
    arguments: a TypeError for every update."""
    fake = FakeOrderExchange()

    class PlacesOnly(lrvx.Strategy):
        def __init__(self, symbols):
            super().__init__(symbols)
            self.sent = False

        def on_trade(self, ctx, trade):
            if not self.sent:
                self.sent = True
                self.limit_buy(99.0, 1.0)

    async def script(fake, strategy, broker):
        fake.push("100")
        await asyncio.sleep(0.02)
        fake.fill("100", 1.0, 99.0)

    _, _, errors = _drive(fake, PlacesOnly, script, streams=("trades", "orders"))
    assert errors == []


def test_fills_go_to_on_fill_and_everything_else_to_on_order_update():
    fake = FakeOrderExchange()

    async def script(fake, strategy, broker):
        fake.fill("100", 0.4, 99.0)
        await asyncio.sleep(0.02)
        fake.push("100")  # the same state again: nothing new to report
        await asyncio.sleep(0.02)
        fake.fill("100", 0.6, 98.0)

    strategy, broker, errors = _drive(
        fake, lambda syms: Recording(syms, lambda s: s.limit_buy(99.0, 1.0)), script)

    assert errors == []
    oid = strategy.order_id
    assert strategy.updates == [(oid, "ACCEPTED")]
    assert [f[:4] for f in strategy.fills] == [
        (oid, "PARTIALLY_FILLED", "buy", pytest.approx(0.4)),
        (oid, "FILLED", "buy", pytest.approx(0.6)),
    ]
    # Each fill's own price, worked back from the running average.
    assert [f[4] for f in strategy.fills] == [pytest.approx(99.0), pytest.approx(98.0)]
    # ctx.position inside on_fill counts the fill.
    assert [f[5] for f in strategy.fills] == [pytest.approx(0.4), pytest.approx(1.0)]
    assert broker._order_progress == {} and broker._lrvx_to_ccxt_order == {}


@pytest.mark.parametrize("ending, status", [
    ("canceled", "CANCELED"), ("expired", "EXPIRED"), ("rejected", "REJECTED")])
def test_an_order_the_exchange_ends_reaches_on_order_update(ending, status):
    fake = FakeOrderExchange()

    async def script(fake, strategy, broker):
        fake.end("100", ending)

    strategy, _, errors = _drive(
        fake, lambda syms: Recording(syms, lambda s: s.limit_buy(99.0, 1.0)), script)

    assert errors == []
    assert strategy.updates == [(strategy.order_id, "ACCEPTED"), (strategy.order_id, status)]
    assert strategy.fills == []


def test_a_market_order_filled_in_its_answer_is_one_fill():
    """Reported from the answer to the placement; the stream's repeat of the
    finished order adds nothing."""
    fake = FakeOrderExchange(fill_market_at=100.5)

    async def script(fake, strategy, broker):
        fake.push("100")

    strategy, _, errors = _drive(fake, Recording, script)

    assert errors == []
    assert strategy.fills == [
        (strategy.order_id, "FILLED", "buy", pytest.approx(1.0), pytest.approx(100.5),
         pytest.approx(1.0))]
    assert strategy.updates == []


def _backtest_fills():
    reg = lrvx.SymbolRegistry()
    sym = reg.add_symbol("test", "BTCUSDT", tick_size=0.01)
    runner = lrvx.BacktestRunner(reg, 0.0, 10_000.0)
    strategy = Recording([sym])
    runner.set_strategy(strategy)
    ts = np.arange(3, dtype=np.int64) * 1_000_000_000 + T0
    runner.run_ohlcv(ts, np.full(3, 100.5), "BTCUSDT")
    return strategy


def _paper_fills():
    reg = lrvx.SymbolRegistry()
    sym = reg.add_symbol("test", "BTCUSDT", tick_size=0.01)
    broker = PaperBroker(reg)
    strategy = Recording([sym])
    broker.set_strategy(strategy)
    broker.start()
    for i in range(3):
        broker.observe_trade(sym, 100.5, 1.0, True, T0 + i * 1_000_000_000)
    broker.stop()
    return strategy


def test_one_strategy_class_hears_its_fill_the_same_way_in_all_three():
    """Backtest, paper and CcxtBroker: the fill arrives in on_fill with the
    order's id, side, quantity, price and the position after it, and never in
    on_order_update."""
    live, _, errors = _drive(FakeOrderExchange(fill_market_at=100.5), Recording, _nothing)
    assert errors == []

    for strategy in (_backtest_fills(), _paper_fills(), live):
        assert len(strategy.fills) == 1, strategy.fills
        order_id, status, side, qty, price, position = strategy.fills[0]
        assert order_id == strategy.order_id
        assert (status, side) == ("FILLED", "buy")
        assert qty == pytest.approx(1.0) and price == pytest.approx(100.5)
        assert position == pytest.approx(1.0)
        assert all(s not in ("FILLED", "PARTIALLY_FILLED") for _, s in strategy.updates)


def test_the_orders_stream_starts_when_the_broker_places_an_order():
    """With the default streams nothing watched the orders: a resting order
    was never reported filled and never left the broker's maps."""
    fake = FakeOrderExchange()

    async def script(fake, strategy, broker):
        assert fake.orders_watched >= 1, "placing an order opened the stream"
        assert list(broker._lrvx_to_ccxt_order.values()) == ["100"]
        fake.fill("100", 1.0, 99.0)

    strategy, broker, errors = _drive(
        fake, lambda syms: Recording(syms, lambda s: s.limit_buy(99.0, 1.0)), script)

    assert errors == []
    assert [f[1] for f in strategy.fills] == ["FILLED"]
    assert broker._lrvx_to_ccxt_order == {} and broker._placed_orders == {}


def test_a_broker_that_places_nothing_never_opens_the_orders_stream():
    fake = FakeOrderExchange()
    _drive(fake, lambda syms: Recording(syms, lambda s: 0), _nothing)
    assert fake.orders_watched == 0


@pytest.mark.parametrize("bare", [False, True], ids=["full answer", "bare answer"])
def test_cancel_all_closes_the_orders_it_cancelled(bare):
    """It dropped them from the broker's maps and told nobody; and it dropped
    the ones whose cancel had failed as well."""
    fake = FakeOrderExchange(bare_cancel_answer=bare)
    closed = []

    class TwoThenCancelAll(Recording):
        def on_trade(self, ctx, trade):
            if self.order_id is None:
                self.order_id = self.limit_buy(99.0, 1.0)
                self.second = self.limit_buy(98.0, 1.0)

    async def script(fake, strategy, broker):
        broker._runner = _ClosedSpy(broker._runner, closed)
        fake.fail_cancel.add("101")
        strategy.cancel_all_orders()
        await asyncio.sleep(0.02)
        assert closed == [strategy.order_id]
        assert broker._lrvx_to_ccxt_order == {strategy.second: "101"}, \
            "the order whose cancel failed is still tracked"

    strategy, broker, errors = _drive(fake, TwoThenCancelAll, script)

    assert [e[0] for e in errors] == ["BTC/USDT"], errors
    assert (strategy.order_id, "CANCELED") in strategy.updates
    assert (strategy.second, "CANCELED") not in strategy.updates


class _ClosedSpy:
    """The broker's runner, with order_closed calls written down."""

    def __init__(self, runner, closed):
        self._runner = runner
        self._closed = closed

    def order_closed(self, order_id):
        self._closed.append(order_id)
        self._runner.order_closed(order_id)

    def __getattr__(self, name):
        return getattr(self._runner, name)


def test_the_four_argument_on_order_update_still_gets_its_call():
    fake = FakeOrderExchange()
    seen = []

    class Old(lrvx.Strategy):
        def __init__(self, symbols):
            super().__init__(symbols)
            self.sent = False
            self.fills = []

        def on_trade(self, ctx, trade):
            if not self.sent:
                self.sent = True
                self.limit_buy(99.0, 1.0)

        def on_order_update(self, sym, status, filled, avg):
            seen.append((sym, status, filled, avg))

        def on_fill(self, ctx, event):
            self.fills.append(event.status)

    async def script(fake, strategy, broker):
        fake.fill("100", 1.0, 99.0)

    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always")
        strategy, _, errors = _drive(fake, Old, script)

    assert errors == []
    assert seen == [("BTC/USDT", "closed", 1.0, 99.0)]
    assert strategy.fills == ["FILLED"]
    assert any(issubclass(w.category, DeprecationWarning) and "on_order_update" in str(w.message)
               for w in caught)


def test_runner_on_order_event_refuses_names_it_does_not_know():
    reg = lrvx.SymbolRegistry()
    sym = reg.add_symbol("test", "BTCUSDT", tick_size=0.01)
    runner = lrvx.Runner(reg, lambda s: None)
    for kwargs, word in (({"side": "long", "status": "FILLED"}, "long"),
                         ({"side": "buy", "status": "DONE"}, "DONE"),
                         ({"side": "buy", "status": "FILLED", "order_type": "lmt"}, "lmt")):
        with pytest.raises(ValueError) as excinfo:
            runner.on_order_event(1, sym, **kwargs)
        assert word in str(excinfo.value)
