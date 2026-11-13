"""Reported order events: whose position counts them and which thread runs
the callbacks.

One position tracker per runner made every strategy on it read the same net
position, and a threaded runner ran ``on_fill`` inside ``on_order_event``,
on whatever thread a broker reported from.
"""
from __future__ import annotations

import threading
import time

import pytest

import lrvx


class Listening(lrvx.Strategy):
    def __init__(self, symbols, trade_delay=0.0):
        super().__init__(symbols)
        self.trade_delay = trade_delay
        self.heard = []
        self.lock = threading.Lock()

    def note(self, kind, position):
        with self.lock:
            self.heard.append((kind, position, threading.get_ident()))

    def on_trade(self, ctx, trade):
        if self.trade_delay:
            time.sleep(self.trade_delay)
        self.note("trade", ctx.position)

    def on_fill(self, ctx, event):
        self.note("fill", ctx.position)


def _runner(threaded, delays=(0.0, 0.0)):
    registry = lrvx.SymbolRegistry()
    btc = registry.add_symbol("test", "BTC", 0.01)
    runner = lrvx.Runner(registry, lambda signal: None, threaded=threaded)
    strategies = [Listening([btc], delays[0]), Listening([btc], delays[1])]
    for s in strategies:
        runner.add_strategy(s)
    return registry, btc, runner, strategies


@pytest.mark.parametrize("threaded", [False, True])
def test_a_fill_counts_for_the_strategy_that_issued_the_order(threaded):
    registry, btc, runner, (first, second) = _runner(threaded)
    runner.start()
    try:
        order = second.limit_buy(100.0, 2.0)
        runner.on_order_event(order, btc, "buy", "FILLED", fill_qty=2.0, fill_price=100.0)
        runner.drain()
        assert second.position() == 2.0
        assert first.position() == 0.0
        assert [(k, p) for k, p, _ in second.heard] == [("fill", 2.0)]
        assert first.heard == []
    finally:
        runner.stop()


def test_a_threaded_runner_runs_the_callbacks_off_the_calling_thread():
    registry, btc, runner, (first, _) = _runner(True)
    runner.start()
    try:
        order = first.limit_buy(100.0, 1.0)
        runner.on_order_event(order, btc, "buy", "FILLED", fill_qty=1.0, fill_price=100.0)
        runner.drain()
        assert [k for k, _, _ in first.heard] == ["fill"]
        assert first.heard[0][2] != threading.get_ident()
    finally:
        runner.stop()


def test_a_fill_arrives_after_the_trades_published_before_it():
    registry, btc, runner, (first, _) = _runner(True, delays=(0.002, 0.0))
    runner.start()
    try:
        order = first.limit_buy(100.0, 1.0)
        for i in range(15):
            runner.on_trade(btc, 100.0, 1.0, True, 1_000 + i)
        runner.on_order_event(order, btc, "buy", "FILLED", fill_qty=1.0, fill_price=100.0)
        runner.drain()
        kinds = [k for k, _, _ in first.heard]
        assert kinds == ["trade"] * 15 + ["fill"]
    finally:
        runner.stop()


def test_drain_on_a_synchronous_runner_returns_at_once():
    registry, btc, runner, _ = _runner(False)
    runner.drain()
    runner.start()
    runner.drain()
    runner.stop()
