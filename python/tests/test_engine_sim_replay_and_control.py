"""`lrvx engine sim`: triggers reach the simulator, the tape's books fill
resting orders, the control server acts through the bridge, and the bridge
works under a threaded runner.

Four things were wrong. The bridge submitted a stop, take-profit or trailing
order without its trigger or offset. The replay fed the tape's trades and
nothing else, so a resting limit order, which fills from the book, never
filled. The ControlServer held the simulator itself: it called it from its
own thread with no lock, and an order it cancelled stayed in the runner's
record and in the bridge's accounting. And nothing ran the bridge under
``--threaded``.
"""
from __future__ import annotations

import threading

import pytest

import lrvx
from lrvx import engine_cli, tape

T0 = 1_000_000_000


def _registry():
    reg = lrvx.SymbolRegistry()
    sym = reg.add_symbol("binance", "BTCUSDT", tick_size=0.01)
    return reg, sym


def _order(order_id, order_type, *, side="sell", price=0.0, quantity=1.0, trigger=0.0,
           offset=0.0, bps=0):
    order = lrvx.Order()
    order.id = order_id
    order.side = side
    order.price = price
    order.quantity = quantity
    order.order_type = order_type
    order.symbol = 1
    order.time_in_force = "gtc"
    order.trigger_price = trigger
    order.trailing_offset = offset
    order.trailing_bps = bps
    return order


class RecordingSim:
    def __init__(self, lock_of=None):
        self.calls = []
        self.fill_count = 0
        self.lock_of = lock_of
        self.unlocked = []

    def _note(self, call):
        self.calls.append(call)
        if self.lock_of is not None and not self.lock_of().locked():
            self.unlocked.append(call[0])

    def submit_order(self, order_id, side, price, quantity, **kwargs):
        self._note(("submit_order", order_id, side, price, quantity, kwargs))

    def cancel_order(self, order_id):
        self._note(("cancel_order", order_id))

    def cancel_all(self, symbol):
        self._note(("cancel_all", symbol))

    def fills_list(self):
        return []


class RunnerSpy:
    def __init__(self, inner):
        self._inner = inner
        self.closed = []

    def order_closed(self, order_id):
        self.closed.append(order_id)
        self._inner.order_closed(order_id)

    def __getattr__(self, name):
        return getattr(self._inner, name)


# ── triggers and offsets ─────────────────────────────────────────────


def test_a_stop_reaches_the_simulator_with_its_trigger():
    sim = RecordingSim()
    bridge = engine_cli._make_sim_bridge(sim)
    bridge.submit(_order(1, "stop_market", trigger=95.0))
    bridge.submit(_order(2, "stop_limit", price=94.5, trigger=95.0))
    bridge.submit(_order(3, "trailing_stop", offset=2.5))

    kwargs = [c[5] for c in sim.calls]
    assert [k["trigger"] for k in kwargs] == [95.0, 95.0, 0.0]
    assert kwargs[2]["trailing_offset"] == 2.5
    assert sim.calls[1][3] == 94.5, "a stop-limit keeps its limit price beside the trigger"


def test_a_trailing_stop_with_neither_offset_nor_rate_is_refused():
    sim = RecordingSim()
    bridge = engine_cli._make_sim_bridge(sim)
    with pytest.raises(ValueError) as excinfo:
        bridge.submit(_order(4, "trailing_stop"))
    assert "neither" in str(excinfo.value)
    assert sim.calls == []


def test_a_percent_trailing_stop_reaches_the_simulator_with_its_rate():
    """The order a runner hands an executor had no field for the rate, so
    the bridge refused every trailing_stop_percent."""
    sim = RecordingSim()
    bridge = engine_cli._make_sim_bridge(sim)
    bridge.submit(_order(5, "trailing_stop", bps=150))
    kwargs = sim.calls[0][5]
    assert kwargs["trailing_bps"] == 150
    assert kwargs["trailing_offset"] == 0.0


class TrailsByPercent(lrvx.Strategy):
    def __init__(self, symbols):
        super().__init__(symbols)
        self.order_id = None

    def on_trade(self, ctx, trade):
        if self.order_id is None:
            self.order_id = self.trailing_stop_percent("sell", 200, 1.0)


class Collecting(lrvx.Executor):
    def __init__(self):
        super().__init__()
        self.orders = []

    def submit(self, order):
        self.orders.append((order.order_type, order.trailing_offset, order.trailing_bps))


def test_an_executor_reads_the_rate_of_the_order_it_is_handed():
    reg, sym = _registry()
    runner = lrvx.Runner(reg, lambda signal: None)
    executor = Collecting()
    runner.set_executor(executor)
    strategy = TrailsByPercent([sym])
    runner.add_strategy(strategy)
    runner.start()
    runner.on_trade(sym, 100.0, 1.0, True, T0)
    runner.stop()
    assert executor.orders == [("trailing_stop", 0.0, 200)]


def test_a_percent_trailing_stop_trails_in_the_real_simulator():
    reg, sym = _registry()
    sim = lrvx.SimulatedExecutor()
    bridge = engine_cli._make_sim_bridge(sim)
    runner = lrvx.Runner(reg, lambda signal: None)
    runner.set_executor(bridge)
    strategy = TrailsByPercent([sym])
    runner.add_strategy(strategy)
    runner.start()
    sim.on_best_levels(int(sym), 100.0, 5.0, 100.5, 5.0)
    runner.on_trade(sym, 100.0, 1.0, True, T0)
    assert strategy.order_id, lrvx.last_error()
    sim.on_trade(int(sym), 110.0, True)
    assert sim.fill_count == 0, "the stop trails 2% under the high"
    sim.on_trade(int(sym), 107.0, False)
    assert sim.fill_count == 1, "110 less 2% is 107.8"
    runner.stop()


def test_a_stop_triggers_where_it_says_in_the_real_simulator():
    sim = lrvx.SimulatedExecutor()
    bridge = engine_cli._make_sim_bridge(sim)
    sim.on_best_levels(1, 100.0, 5.0, 100.5, 5.0)
    bridge.submit(_order(1, "stop_market", trigger=95.0))
    sim.on_trade(1, 97.0, False)
    assert sim.fill_count == 0, "above the trigger"
    sim.on_trade(1, 94.0, False)
    assert sim.fill_count == 1


# ── the tape's books ─────────────────────────────────────────────────


def _write_tape(directory, with_books=True):
    reg, sym = _registry()
    recorder = tape.make_recorder_hook(directory)
    runner = lrvx.Runner(reg, on_signal=lambda _: None)
    runner.set_market_data_recorder(recorder)
    runner.start()
    runner.on_trade(sym, 100.00, 1.0, True, T0)
    if with_books:
        runner.on_book_snapshot(sym, [99.5], [5.0], [100.0], [5.0], 2 * T0)
        runner.on_book_snapshot(sym, [98.0], [5.0], [98.5], [5.0], 3 * T0)
    else:
        runner.on_trade(sym, 100.50, 0.5, False, 2 * T0)
        runner.on_trade(sym, 101.00, 1.5, True, 3 * T0)
    runner.stop()
    recorder.close()
    return int(sym)


class LimitBuyOnFirstTrade(lrvx.Strategy):
    def __init__(self, symbols):
        super().__init__(symbols)
        self.order_id = 0
        self.books = 0
        self.submitted = threading.Event()

    def on_trade(self, ctx, trade):
        if self.order_id == 0:
            self.order_id = self.limit_buy(99.0, 1.0)
            self.submitted.set()

    def on_book_update(self, ctx):
        self.books += 1


def test_a_resting_limit_order_fills_from_the_tapes_books(tmp_path):
    tape_dir = tmp_path / "tape"
    tape_dir.mkdir()
    tape_sym = _write_tape(tape_dir)

    reg, sym = _registry()
    sim = lrvx.SimulatedExecutor()
    bridge = engine_cli._make_sim_bridge(sim)
    spy = RunnerSpy(lrvx.Runner(reg, on_signal=lambda s: None))
    spy.set_executor(bridge)
    strat = LimitBuyOnFirstTrade([sym])
    spy.add_strategy(strat)
    spy.start()
    delivered = engine_cli._feed_tape(spy, tape_dir, tape_sym, int(sym), bridge)
    spy.stop()

    assert delivered == 1, "one trade on the tape"
    assert strat.books == 2, "the strategy sees the books as well"
    fills = sim.fills_list()
    assert [(f["order_id"], f["quantity"]) for f in fills] == [(strat.order_id, 1.0)]
    assert fills[0]["price"] <= 99.0
    assert spy.closed == [strat.order_id]
    assert not bridge.lock.locked()


def test_replay_tape_delivers_only_the_chosen_symbol_under_the_registered_id(tmp_path):
    tape_dir = tmp_path / "tape"
    tape_dir.mkdir()
    tape_sym = _write_tape(tape_dir)

    reg = lrvx.SymbolRegistry()
    reg.add_symbol("binance", "OTHER", tick_size=0.01)
    sym = reg.add_symbol("binance", "BTCUSDT", tick_size=0.01)
    assert int(sym) != tape_sym
    seen = []

    class Watch(lrvx.Strategy):
        def on_trade(self, ctx, trade):
            seen.append(("trade", trade.price))

        def on_book_update(self, ctx):
            seen.append(("book", None))

    runner = lrvx.Runner(reg, on_signal=lambda s: None)
    runner.add_strategy(Watch([sym]))
    runner.start()
    sim = lrvx.SimulatedExecutor()
    stats = runner.replay_tape(str(tape_dir), sim, tape_symbol_id=tape_sym, symbol=sym)
    none = runner.replay_tape(str(tape_dir), sim, tape_symbol_id=tape_sym + 7, symbol=sym)
    runner.stop()

    assert (stats["events"], stats["trades"], stats["books"]) == (3, 1, 2)
    assert [kind for kind, _ in seen] == ["trade", "book", "book"]
    assert none["events"] == 0

    with pytest.raises(ValueError):
        runner.replay_tape(str(tape_dir), sim, tape_symbol_id=tape_sym)
    with pytest.raises(ValueError):
        runner.replay_tape(str(tmp_path / "absent"), sim)


# ── the control server's orders ──────────────────────────────────────


def test_control_orders_go_through_the_bridge_under_its_lock():
    box = {}
    sim = RecordingSim(lock_of=lambda: box["bridge"].lock)
    bridge = box["bridge"] = engine_cli._make_sim_bridge(sim)
    reg, sym = _registry()
    spy = RunnerSpy(lrvx.Runner(reg, on_signal=lambda s: None))
    bridge.control.attach_runner(spy)

    bridge.submit(_order(7, "limit", side="buy", price=99.0))
    bridge.control.submit_order(21, "buy", 98.0, 2.0, type="limit", symbol=1)
    assert sorted(bridge._working) == [7, 21], "both are accounted as working"

    bridge.control.cancel_order(7)
    assert spy.closed == [7], "the runner is told the order is finished"
    assert sorted(bridge._working) == [21]

    bridge.control.cancel_order(999)
    assert spy.closed == [7], "an order the bridge never had is not reported"

    bridge.submit(_order(8, "limit", side="buy", price=97.0))
    bridge.control.cancel_all(1)
    assert sorted(spy.closed) == [7, 8, 21]
    assert bridge._working == {}

    assert [c[0] for c in sim.calls] == [
        "submit_order", "submit_order", "cancel_order", "cancel_order", "submit_order",
        "cancel_all"]
    assert sim.unlocked == [], "every call into the simulator held the bridge's lock"


class LimitThenModify(lrvx.Strategy):
    def __init__(self, symbols):
        super().__init__(symbols)
        self.order_id = 0
        self.errors = []

    def on_trade(self, ctx, trade):
        if self.order_id == 0:
            self.order_id = self.limit_buy(90.0, 1.0)
        else:
            self.modify_order(self.order_id, 91.0, 1.0)
            self.errors.append(lrvx.last_error())


def test_a_control_cancel_is_reflected_in_the_runners_record():
    """A strategy's modify of an order the control server cancelled used to
    go out as a fresh order: the runner still held it."""
    reg, sym = _registry()
    sim = lrvx.SimulatedExecutor()
    bridge = engine_cli._make_sim_bridge(sim)
    runner = lrvx.Runner(reg, on_signal=lambda s: None)
    bridge.control.attach_runner(runner)
    runner.set_executor(bridge)
    strat = LimitThenModify([sym])
    runner.add_strategy(strat)
    runner.start()

    sim.on_best_levels(int(sym), 100.0, 5.0, 100.5, 5.0)
    runner.on_trade(sym, 100.2, 1.0, True, T0)
    bridge.control.cancel_order(strat.order_id)
    runner.on_trade(sym, 100.2, 1.0, True, T0 + 1)
    runner.stop()

    assert len(strat.errors) == 1 and strat.errors[0].endswith("[unknown_order]"), strat.errors
    assert bridge._working == {}


def test_a_fill_of_a_control_order_is_accounted():
    reg, sym = _registry()
    sim = lrvx.SimulatedExecutor()
    bridge = engine_cli._make_sim_bridge(sim)
    spy = RunnerSpy(lrvx.Runner(reg, on_signal=lambda s: None))
    bridge.control.attach_runner(spy)

    sim.on_best_levels(1, 100.0, 5.0, 100.5, 5.0)
    bridge.control.submit_order(21, "buy", 0.0, 1.0, type="market", symbol=1)
    assert sim.fill_count == 1
    assert bridge.report_fills(spy) == 1
    assert bridge._working == {}


def test_engine_sim_hands_the_control_server_the_bridge():
    import inspect

    source = inspect.getsource(engine_cli.cmd_engine_sim)
    assert "executor=sim_bridge.control" in source
    assert "executor=sim," not in source


# ── --threaded ───────────────────────────────────────────────────────


class WaitsForTheStrategy(RunnerSpy):
    """The replay does not wait for a threaded runner's strategy thread. So
    that the test is not a race, the first event's hook waits until the
    strategy has sent its order."""

    def __init__(self, inner, strategy):
        super().__init__(inner)
        self._strategy = strategy
        self.waited = None

    def replay_tape(self, path, simulator, **kwargs):
        after_event = kwargs.pop("after_event")

        def hook():
            if self.waited is None:
                self.waited = self._strategy.submitted.wait(10.0)
            after_event()

        return self._inner.replay_tape(path, simulator, after_event=hook, **kwargs)


@pytest.mark.parametrize("with_books", [True, False], ids=["books", "trades"])
def test_the_bridge_under_a_threaded_runner(tmp_path, with_books):
    tape_dir = tmp_path / "tape"
    tape_dir.mkdir()
    tape_sym = _write_tape(tape_dir, with_books=with_books)

    reg, sym = _registry()
    sim = lrvx.SimulatedExecutor()
    bridge = engine_cli._make_sim_bridge(sim)
    strat = LimitBuyOnFirstTrade([sym])
    if not with_books:
        strat.limit_buy = lambda price, qty: strat.market_buy(qty)
    spy = WaitsForTheStrategy(lrvx.Runner(reg, on_signal=lambda s: None, threaded=True), strat)
    bridge.control.attach_runner(spy)
    spy.set_executor(bridge)
    spy.add_strategy(strat)
    spy.start()
    try:
        delivered = engine_cli._feed_tape(spy, tape_dir, tape_sym, int(sym), bridge)
    finally:
        spy.stop()

    assert spy.waited is True, "the strategy thread sent its order"
    assert delivered == (1 if with_books else 3)
    fills = sim.fills_list()
    assert [(f["order_id"], f["quantity"]) for f in fills] == [(strat.order_id, 1.0)]
    assert spy.closed == [strat.order_id]
    assert bridge._working == {} and not bridge.lock.locked()
