"""run_ohlcv and run_bars without a symbol replay for the first registered one.

They looked up a symbol literally named "default" and raised E_SYM_001, where
the C API (and so Node, Codon and QuickJS) takes the registry's first symbol.
"""
from __future__ import annotations

import numpy as np
import pytest

import lrvx

T0 = 1_788_220_800_000_000_000
STEP = 60_000_000_000
N = 5


class Records(lrvx.Strategy):
    def __init__(self, symbols):
        super().__init__(symbols)
        self.trades = []
        self.bars = []

    def on_trade(self, ctx, trade):
        self.trades.append((int(trade.symbol), trade.price))

    def on_bar(self, ctx, bar):
        self.bars.append((int(bar.symbol), bar.close))


def _runner():
    reg = lrvx.SymbolRegistry()
    first = reg.add_symbol("test", "ETHUSDT", tick_size=0.01)
    second = reg.add_symbol("test", "BTCUSDT", tick_size=0.01)
    strat = Records([first, second])
    runner = lrvx.BacktestRunner(reg, fee_rate=0.0, initial_capital=10_000.0)
    runner.set_strategy(strat)
    return runner, strat, int(first), int(second)


def _ohlcv():
    ts = np.array([T0 + i * STEP for i in range(N)], dtype=np.int64)
    close = np.array([100.0 + i for i in range(N)])
    return ts, close


def _bars():
    start = np.array([T0 + i * STEP for i in range(N)], dtype=np.int64)
    end = start + STEP
    close = np.array([100.0 + i for i in range(N)])
    return start, end, close, close + 0.5, close - 0.5, close, np.full(N, 10.0)


def test_run_ohlcv_without_a_symbol_uses_the_first_registered():
    runner, strat, first, _second = _runner()
    stats = runner.run_ohlcv(*_ohlcv())
    assert stats is not None
    assert len(strat.trades) == N
    assert {sym for sym, _ in strat.trades} == {first}


def test_run_ohlcv_with_an_empty_symbol_is_the_same():
    runner, strat, first, _second = _runner()
    runner.run_ohlcv(*_ohlcv(), "")
    assert {sym for sym, _ in strat.trades} == {first}


def test_run_bars_without_a_symbol_uses_the_first_registered():
    runner, strat, first, _second = _runner()
    stats = runner.run_bars(*_bars())
    assert stats is not None
    assert len(strat.bars) == N
    assert {sym for sym, _ in strat.bars} == {first}


def test_a_named_symbol_is_still_the_one_replayed():
    runner, strat, _first, second = _runner()
    runner.run_ohlcv(*_ohlcv(), "BTCUSDT")
    assert {sym for sym, _ in strat.trades} == {second}

    runner, strat, _first, second = _runner()
    runner.run_bars(*_bars(), symbol="BTCUSDT")
    assert {sym for sym, _ in strat.bars} == {second}


def test_an_unknown_name_is_still_refused():
    runner, _strat, _first, _second = _runner()
    with pytest.raises(lrvx.LrvxError) as err:
        runner.run_ohlcv(*_ohlcv(), "default")
    assert "default" in str(err.value)
    with pytest.raises(lrvx.LrvxError):
        runner.run_bars(*_bars(), symbol="NOPE")
