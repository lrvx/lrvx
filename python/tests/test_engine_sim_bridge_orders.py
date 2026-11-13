"""`lrvx engine sim`: a modify reaches the simulator, and the runner is told
when the simulator finishes an order.

The bridge between the runner and the SimulatedExecutor had no `replace`, so
a strategy's modify_order was dropped, and nothing reported a fill to
Runner.order_closed, so the runner's record of forwarded orders kept every
order that filled. The tape replay fed its trades to the runner alone: the
simulator never saw a price and never filled anything.
"""
from __future__ import annotations

import lrvx
from lrvx import engine_cli, tape

T0 = 1_788_220_800_000_000_000


class RecordingSim:
    """Stands in for the simulator where the calls themselves are the point."""

    def __init__(self):
        self.calls = []
        self.fill_count = 0

    def submit_order(self, order_id, side, price, quantity, **kwargs):
        self.calls.append(("submit_order", order_id, side, price, quantity, kwargs))

    def cancel_order(self, order_id):
        self.calls.append(("cancel_order", order_id))

    def cancel_all(self, symbol):
        self.calls.append(("cancel_all", symbol))

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


def _order(order_id, price, quantity, order_type="limit", side="buy"):
    order = lrvx.Order()
    order.id = order_id
    order.side = side
    order.price = price
    order.quantity = quantity
    order.order_type = order_type
    order.symbol = 1
    order.time_in_force = "gtc"
    return order


def _registry():
    reg = lrvx.SymbolRegistry()
    sym = reg.add_symbol("binance", "BTCUSDT", tick_size=0.01)
    return reg, sym


def test_replace_cancels_the_order_and_submits_it_again_under_its_id():
    sim = RecordingSim()
    bridge = engine_cli._make_sim_bridge(sim)
    bridge.submit(_order(7, 100.0, 1.0))
    bridge.replace(7, _order(7, 101.5, 0.4))

    assert [c[:2] for c in sim.calls] == [
        ("submit_order", 7), ("cancel_order", 7), ("submit_order", 7)]
    assert sim.calls[2][3:5] == (101.5, 0.4)
    assert sim.calls[2][5]["type"] == "limit"


class LimitThenModify(lrvx.Strategy):
    def __init__(self, symbols):
        super().__init__(symbols)
        self.order_id = 0
        self.errors = []

    def on_trade(self, ctx, trade):
        if self.order_id == 0:
            self.order_id = self.limit_buy(99.0, 1.0)
        else:
            self.modify_order(self.order_id, 98.0, 2.0)
            self.errors.append(lrvx.last_error())


def test_a_strategys_modify_reaches_the_simulator_through_the_runner():
    reg, sym = _registry()
    sim = RecordingSim()
    runner = lrvx.Runner(reg, on_signal=lambda s: None)
    runner.set_executor(engine_cli._make_sim_bridge(sim))
    strat = LimitThenModify([sym])
    runner.add_strategy(strat)
    runner.start()
    runner.on_trade(sym, 100.0, 1.0, True, T0)
    runner.on_trade(sym, 100.0, 1.0, True, T0 + 1)
    runner.stop()

    assert strat.errors == [""]
    assert [c[:2] for c in sim.calls] == [
        ("submit_order", strat.order_id), ("cancel_order", strat.order_id),
        ("submit_order", strat.order_id)]
    assert sim.calls[2][3:5] == (98.0, 2.0)


def test_a_fill_is_reported_to_the_runner_once_the_order_is_complete():
    reg, sym = _registry()
    sim = lrvx.SimulatedExecutor()
    bridge = engine_cli._make_sim_bridge(sim)
    spy = RunnerSpy(lrvx.Runner(reg, on_signal=lambda s: None))

    sim.on_best_levels(1, 100.5, 5.0, 101.0, 5.0)
    bridge.submit(_order(7, 100.0, 3.0))
    bridge.submit(_order(8, 90.0, 1.0))
    assert bridge.report_fills(spy) == 0
    assert spy.closed == []

    sim.on_best_levels(1, 99.0, 5.0, 99.5, 5.0)
    assert sim.fill_count == 1
    assert bridge.report_fills(spy) == 1
    assert spy.closed == [7], "the order that filled, not the one still resting"
    assert bridge.report_fills(spy) == 0, "a fill is reported once"
    assert spy.closed == [7]


class LimitThenModifyAfterFill(lrvx.Strategy):
    def __init__(self, symbols):
        super().__init__(symbols)
        self.order_id = 0
        self.errors = []

    def on_trade(self, ctx, trade):
        if self.order_id == 0:
            self.order_id = self.limit_buy(100.0, 1.0)
        else:
            self.modify_order(self.order_id, 99.0, 1.0)
            self.errors.append(lrvx.last_error())


def test_a_modify_after_the_fill_is_refused_by_the_runner():
    """What the report is for: the runner no longer holds the filled order,
    so a late modify is refused there and never sent as a new order."""
    reg, sym = _registry()
    sim = lrvx.SimulatedExecutor()
    bridge = engine_cli._make_sim_bridge(sim)
    runner = lrvx.Runner(reg, on_signal=lambda s: None)
    runner.set_executor(bridge)
    strat = LimitThenModifyAfterFill([sym])
    runner.add_strategy(strat)
    runner.start()

    sim.on_best_levels(int(sym), 100.5, 5.0, 101.0, 5.0)
    runner.on_trade(sym, 100.8, 1.0, True, T0)
    sim.on_best_levels(int(sym), 99.0, 5.0, 99.5, 5.0)
    assert sim.fill_count == 1
    assert bridge.report_fills(runner) == 1
    runner.on_trade(sym, 99.2, 1.0, True, T0 + 1)
    runner.stop()

    assert len(strat.errors) == 1
    assert strat.errors[0].endswith("[unknown_order]"), strat.errors[0]
    assert sim.fill_count == 1, "nothing new reached the simulator"


class BuyOnFirstTrade(lrvx.Strategy):
    def __init__(self, symbols):
        super().__init__(symbols)
        self.order_id = 0

    def on_trade(self, ctx, trade):
        if self.order_id == 0:
            self.order_id = self.market_buy(1.0)


def _write_tape(directory):
    reg, sym = _registry()
    recorder = tape.make_recorder_hook(directory)
    runner = lrvx.Runner(reg, on_signal=lambda _: None)
    runner.set_market_data_recorder(recorder)
    runner.start()
    runner.on_trade(sym, 100.00, 1.0, True, 1_000_000_000)
    runner.on_trade(sym, 100.50, 0.5, False, 2_000_000_000)
    runner.on_trade(sym, 101.00, 1.5, True, 3_000_000_000)
    runner.stop()
    recorder.close()
    return int(sym)


def test_the_tape_replay_feeds_the_simulator_and_reports_its_fills(tmp_path):
    tape_dir = tmp_path / "tape"
    tape_dir.mkdir()
    tape_sym = _write_tape(tape_dir)

    reg, sym = _registry()
    sim = lrvx.SimulatedExecutor()
    bridge = engine_cli._make_sim_bridge(sim)
    spy = RunnerSpy(lrvx.Runner(reg, on_signal=lambda s: None))
    spy.set_executor(bridge)
    strat = BuyOnFirstTrade([sym])
    spy.add_strategy(strat)
    spy.start()
    delivered = engine_cli._feed_tape(spy, tape_dir, tape_sym, int(sym), bridge)
    spy.stop()

    assert delivered == 3
    fills = sim.fills_list()
    assert [(f["order_id"], f["quantity"]) for f in fills] == [(strat.order_id, 1.0)]
    assert spy.closed == [strat.order_id]
