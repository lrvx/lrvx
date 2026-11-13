"""``lrvx engine sim``: what the strategy hears of its fills, what the state
file says, a threaded replay, and when the control server opens.

Four things were wrong. A simulator fill reached ``Runner.order_closed`` and
nothing else, so the strategy's ``on_fill`` never ran and it read a position
of 0. The state file always said no positions, no open orders and no PnL.
With ``--threaded`` the replay did not wait for the strategy thread, so an
order met the tape wherever that thread happened to be. And the control
server opened before the replay reset the simulator, so an order placed in
between was wiped.
"""
from __future__ import annotations

import argparse
import json
import threading
import time

import pytest

import lrvx
from lrvx import engine_cli, tape

T0 = 1_000_000_000


def _registry():
    reg = lrvx.SymbolRegistry()
    sym = reg.add_symbol("binance", "BTCUSDT", tick_size=0.01)
    return reg, sym


def _write_tape(directory, prices):
    """One trade per price, a second apart."""
    reg, sym = _registry()
    recorder = tape.make_recorder_hook(directory)
    runner = lrvx.Runner(reg, on_signal=lambda _: None)
    runner.set_market_data_recorder(recorder)
    runner.start()
    for i, price in enumerate(prices):
        runner.on_trade(sym, price, 1.0, True, (i + 1) * T0)
    runner.stop()
    recorder.close()
    return int(sym)


class BuysThenSells(lrvx.Strategy):
    """Buys 2 at market on the first trade and sells 0.5 on the fourth,
    and writes down what it hears."""

    delay = 0.0

    def __init__(self, symbols):
        super().__init__(symbols)
        self.trades = 0
        self.fills = []
        self.updates = []

    def on_trade(self, ctx, trade):
        if self.delay:
            time.sleep(self.delay)
        self.trades += 1
        if self.trades == 1:
            self.market_buy(2.0)
        elif self.trades == 4:
            self.market_sell(0.5)

    def on_fill(self, ctx, event):
        self.fills.append((event.status, event.side, event.fill_qty, event.fill_price,
                           ctx.position))

    def on_order_update(self, ctx, event):
        self.updates.append(event.status)


def _replay(tmp_path, prices, *, threaded=False, delay=0.0, name="tape"):
    tape_dir = tmp_path / name
    tape_dir.mkdir()
    tape_sym = _write_tape(tape_dir, prices)
    reg, sym = _registry()
    sim = lrvx.SimulatedExecutor()
    bridge = engine_cli._make_sim_bridge(sim)
    runner = lrvx.Runner(reg, on_signal=lambda s: None, threaded=threaded)
    bridge.control.attach_runner(runner)
    runner.set_executor(bridge)
    strategy = BuysThenSells([sym])
    strategy.delay = delay
    runner.add_strategy(strategy)
    runner.start()
    try:
        engine_cli._feed_tape(runner, tape_dir, tape_sym, int(sym), bridge,
                              mark=strategy.last_price)
    finally:
        runner.stop()
    return strategy, sim, bridge, int(sym)


PRICES = [100.0, 101.0, 102.0, 103.0, 104.0, 105.0]


# ── fills reach the strategy ─────────────────────────────────────────


def test_a_simulator_fill_reaches_on_fill_with_the_position_counted(tmp_path):
    strategy, sim, bridge, _ = _replay(tmp_path, PRICES)

    assert [(f[0], f[1], f[2]) for f in strategy.fills] == [
        ("FILLED", "buy", 2.0), ("FILLED", "sell", 0.5)]
    assert [f[4] for f in strategy.fills] == [2.0, 1.5], "ctx.position counts each fill"
    assert strategy.position() == 1.5
    assert [f[3] for f in strategy.fills] == [f["price"] for f in sim.fills_list()]
    assert bridge._working == {}


class ScriptedSim:
    """Stands in for the simulator: takes orders and fills what it is told."""

    def __init__(self):
        self.submitted = []
        self._fills = []

    @property
    def fill_count(self):
        return len(self._fills)

    def submit_order(self, order_id, side, price, quantity, **kwargs):
        self.submitted.append((order_id, side, kwargs.get("symbol")))

    def fills_list(self):
        return list(self._fills)

    def fill(self, order_id, symbol, side, price, quantity, ts_ns):
        self._fills.append({"order_id": order_id, "symbol": symbol, "side": side,
                            "price": price, "quantity": quantity, "timestamp_ns": ts_ns,
                            "is_maker": True})


def test_a_partial_fill_is_reported_as_one():
    reg, sym = _registry()
    sim = ScriptedSim()
    bridge = engine_cli._make_sim_bridge(sim)
    runner = lrvx.Runner(reg, on_signal=lambda s: None)
    runner.set_executor(bridge)

    class Resting(lrvx.Strategy):
        def __init__(self, symbols):
            super().__init__(symbols)
            self.order_id = 0
            self.heard = []
            self.closed_modify = None

        def on_trade(self, ctx, trade):
            if not self.order_id:
                self.order_id = self.limit_buy(99.0, 3.0)

        def on_fill(self, ctx, event):
            self.heard.append((event.status, event.fill_qty, event.fill_price, event.is_maker,
                               ctx.position))

    strategy = Resting([sym])
    runner.add_strategy(strategy)
    runner.start()
    runner.on_trade(sym, 100.2, 1.0, True, T0)
    sim.fill(strategy.order_id, int(sym), "buy", 99.0, 1.0, 2 * T0)
    assert bridge.report_fills(runner) == 0, "the order is still working"
    assert bridge.report_fills(runner) == 0, "and the same fill is not reported twice"
    sim.fill(strategy.order_id, int(sym), "buy", 98.5, 2.0, 3 * T0)
    assert bridge.report_fills(runner) == 1
    runner.stop()

    assert strategy.heard == [("PARTIALLY_FILLED", 1.0, 99.0, True, 1.0),
                              ("FILLED", 2.0, 98.5, True, 3.0)]
    assert bridge._working == {}


# ── a threaded replay ────────────────────────────────────────────────


def test_a_threaded_replay_fills_where_the_synchronous_one_does(tmp_path):
    """The strategy thread is slow: without a barrier its first order met
    the tape several events late, or after it had ended."""
    sync, sync_sim, _, _ = _replay(tmp_path, PRICES, name="sync")
    threaded, threaded_sim, _, _ = _replay(tmp_path, PRICES, threaded=True, delay=0.01,
                                           name="threaded")

    def summary(fills):
        return [(f["side"], f["quantity"], f["price"], f["timestamp_ns"]) for f in fills]

    assert len(sync_sim.fills_list()) == 2
    assert summary(threaded_sim.fills_list()) == summary(sync_sim.fills_list())
    assert threaded.fills == sync.fills
    assert threaded.trades == sync.trades == len(PRICES)


# ── the state file ───────────────────────────────────────────────────


def test_the_account_says_what_is_held_what_is_open_and_what_it_made(tmp_path):
    strategy, sim, bridge, sym = _replay(tmp_path, PRICES)
    resting = 77
    bridge.control.submit_order(resting, "buy", 90.0, 1.0, type="limit", symbol=sym)

    account = bridge.account({sym: "BTCUSDT"}, "BuysThenSells")
    buy, sell = (f["price"] for f in sim.fills_list())
    assert account["positions"] == [{
        "account": "sim", "strategy": "BuysThenSells", "symbol_id": sym,
        "symbol_name": "BTCUSDT", "qty": 1.5, "avg_price": pytest.approx(buy),
        "unrealized_pnl": pytest.approx((PRICES[-1] - buy) * 1.5)}]
    assert account["pnl"]["by_strategy"] == [{
        "strategy": "BuysThenSells", "realized": pytest.approx((sell - buy) * 0.5),
        "unrealized": pytest.approx((PRICES[-1] - buy) * 1.5), "fees": 0.0, "trades": 2}]
    assert account["pnl"]["total"]["realized"] == pytest.approx((sell - buy) * 0.5)
    assert [(o["order_id"], o["side"], o["type"], o["qty"], o["price"])
            for o in account["open_orders"]] == [(str(resting), "BUY", "LIMIT", 1.0, 90.0)]


def test_the_state_writer_writes_the_account(tmp_path):
    strategy, sim, bridge, sym = _replay(tmp_path, PRICES)
    path = tmp_path / "state.json"
    writer = engine_cli._StateWriter(
        path=path, kill_switch=engine_cli._KillSwitch(), strategy_name="BuysThenSells",
        symbol_id=sym, symbol_name="BTCUSDT", period_s=10.0,
        account=lambda: bridge.account({sym: "BTCUSDT"}, "BuysThenSells"))
    writer._write_once()
    state = json.loads(path.read_text())

    assert state["schema_version"] == 1
    assert [p["qty"] for p in state["positions"]] == [1.5]
    assert state["pnl"]["by_strategy"][0]["trades"] == 2
    assert state["pnl"]["total"]["unrealized"] != 0.0
    assert state["open_orders"] == []


def test_a_flat_account_lists_no_position(tmp_path):
    reg, sym = _registry()
    bridge = engine_cli._make_sim_bridge(lrvx.SimulatedExecutor())
    account = bridge.account({int(sym): "BTCUSDT"}, "S")
    assert account["positions"] == [] and account["open_orders"] == []
    assert account["pnl"]["total"] == {"realized": 0.0, "unrealized": 0.0, "fees": 0.0}


# ── the control server opens after the reset ─────────────────────────


def test_replay_tape_calls_on_start_after_the_reset_and_before_the_first_event(tmp_path):
    tape_dir = tmp_path / "tape"
    tape_dir.mkdir()
    tape_sym = _write_tape(tape_dir, PRICES)
    reg, sym = _registry()
    sim = lrvx.SimulatedExecutor()
    bridge = engine_cli._make_sim_bridge(sim)
    runner = lrvx.Runner(reg, on_signal=lambda s: None)
    bridge.control.attach_runner(runner)
    runner.set_executor(bridge)
    runner.start()

    # Wiped by the reset: placed before the replay began.
    bridge.control.submit_order(5, "buy", 0.0, 1.0, type="market", symbol=int(sym))
    seen = []

    def on_start():
        seen.append(sim.fill_count)
        bridge.control.submit_order(6, "buy", 0.0, 1.0, type="market", symbol=int(sym))

    runner.replay_tape(str(tape_dir), sim, tape_symbol_id=tape_sym, symbol=int(sym),
                       lock=bridge.lock, on_start=on_start)
    runner.stop()

    assert seen == [0], "called once, with the simulator already reset"
    assert [f["order_id"] for f in sim.fills_list()] == [6], "the order placed in it survives"


class FakeControlServer:
    """Places a market order the moment it opens, as an MCP client might."""

    opened = []

    def __init__(self, **kwargs):
        self._executor = kwargs["executor"]
        self.url = "http://127.0.0.1:0"

    def start(self):
        self._executor.submit_order(900, "buy", 0.0, 1.0, type="market", symbol=1)
        FakeControlServer.opened.append(self)

    def stop(self):
        pass


def test_an_order_placed_as_the_server_opens_is_not_wiped(tmp_path, monkeypatch, capsys):
    from lrvx import control_server

    tape_dir = tmp_path / "tape"
    tape_dir.mkdir()
    _write_tape(tape_dir, PRICES)
    strategy_file = tmp_path / "strat.py"
    strategy_file.write_text(
        "import lrvx\n"
        "class Idle(lrvx.Strategy):\n"
        "    def on_trade(self, ctx, trade):\n"
        "        pass\n")
    FakeControlServer.opened = []
    monkeypatch.setattr(control_server, "ControlServer", FakeControlServer)
    state_path = tmp_path / "state.json"
    args = argparse.Namespace(
        strategy=str(strategy_file), tape=str(tape_dir), state_file=str(state_path),
        exchange="binance", symbol_name="BTCUSDT", tick_size=0.01, tape_symbol_id=None,
        threaded=False, token="t", host="127.0.0.1", port=0)

    held = {}

    def hold(context):
        held["fills"] = context["bridge"].simulator.fills_list()
        context["writer"]._write_once()
        held["state"] = json.loads(state_path.read_text())

    assert engine_cli._engine_sim(args, hold) == 0
    capsys.readouterr()

    assert len(FakeControlServer.opened) == 1
    assert [f["order_id"] for f in held["fills"]] == [900], "filled by the tape, not wiped"
    assert [p["qty"] for p in held["state"]["positions"]] == [1.0]
    assert held["state"]["pnl"]["by_strategy"][0]["trades"] == 1
