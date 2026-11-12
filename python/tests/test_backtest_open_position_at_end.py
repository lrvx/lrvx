"""What a backtest reports when the run ends with a position still open.

``trades()`` and ``equity_curve()`` hold closed trades only. ``total_fees``
counts every fill, so it also holds the entry fee of the position still open
at the end, and ``net_pnl`` (``total_pnl - total_fees``) is lower than the
closed trades' net by exactly that fee. The open position's unrealized PnL is
in none of them. When no trade closed at all, every statistic is zero,
``total_fees`` included.

A caller tells that a position was left open from ``Strategy.position()``:
read it in ``on_stop()``, which the runner calls at the end of the run, or on
the strategy after the run while the runner that ran it is still alive.

This pins the rule documented in docs/reference/python/strategy.md and
backtest.md, so a change to it has to update those pages too.
"""
from __future__ import annotations

import lrvx

FEE_RATE = 0.0004
QTY = 0.01
PRICE = 78_000.0
# The open entry's fee: 0.01 BTC at 78,000 and 4 bp.
OPEN_ENTRY_FEE = QTY * PRICE * FEE_RATE


class _BuySellBuy(lrvx.Strategy):
    """Buys on the first trade, closes on the third, buys again on the fifth
    and holds to the end of the data."""

    def __init__(self, symbols, close_at=2, reopen_at=4):
        super().__init__(symbols)
        self.n = 0
        self.close_at = close_at
        self.reopen_at = reopen_at
        self.position_at_stop = None

    def on_trade(self, ctx, trade):
        if self.n == 0 or self.n == self.reopen_at:
            self.market_buy(QTY)
        elif self.n == self.close_at:
            self.close_position()
        self.n += 1

    def on_stop(self):
        self.position_at_stop = self.position()


def _run(strategy_factory, closes):
    registry = lrvx.SymbolRegistry()
    sym = registry.add_symbol("backtest", "BTCUSDT", tick_size=0.01)
    runner = lrvx.BacktestRunner(registry, fee_rate=FEE_RATE, initial_capital=10_000.0)
    strategy = strategy_factory([sym])
    runner.set_strategy(strategy)
    ts = [1_700_000_000_000_000_000 + i * 60_000_000_000 for i in range(len(closes))]
    stats = runner.run_ohlcv(ts, closes, symbol="BTCUSDT")
    return stats, runner.trades(), runner.equity_curve(), strategy, runner


def test_net_pnl_carries_the_open_entry_fee_and_trades_do_not() -> None:
    # Every price from the reopening on is 78,000, so the open entry fills
    # there whichever bar the market order lands on.
    closes = [PRICE, 78_500.0, 79_000.0, PRICE, PRICE, PRICE, PRICE, PRICE]
    stats, trades, equity, strategy, _runner = _run(_BuySellBuy, closes)

    assert len(trades["pnl"]) == 1
    assert stats["total_trades"] == 1
    closed_net = float(sum(trades["pnl"]) - sum(trades["fee"]))

    assert abs(stats["total_fees"] - float(sum(trades["fee"])) - OPEN_ENTRY_FEE) < 1e-9
    assert abs(stats["net_pnl"] - (closed_net - OPEN_ENTRY_FEE)) < 1e-9
    assert abs(stats["final_capital"] - (10_000.0 + closed_net - OPEN_ENTRY_FEE)) < 1e-9

    # The equity curve stops at the last closed trade: no open entry fee.
    assert len(equity["equity"]) == 1
    assert abs(float(equity["equity"][-1]) - (10_000.0 + closed_net)) < 1e-9

    # How the caller tells a position was left open.
    assert abs(strategy.position_at_stop - QTY) < 1e-12
    assert abs(strategy.position() - QTY) < 1e-12


def test_flat_at_the_end_net_pnl_matches_the_closed_trades() -> None:
    closes = [PRICE, 78_500.0, 79_000.0, PRICE, PRICE, PRICE, PRICE, PRICE]
    stats, trades, _, strategy, _runner = _run(lambda s: _BuySellBuy(s, reopen_at=10_000), closes)

    closed_net = float(sum(trades["pnl"]) - sum(trades["fee"]))
    assert abs(stats["net_pnl"] - closed_net) < 1e-9
    assert abs(stats["total_fees"] - float(sum(trades["fee"]))) < 1e-9
    assert strategy.position_at_stop == 0.0
    assert strategy.position() == 0.0


def test_no_closed_trade_reports_zero_fees_and_pnl() -> None:
    closes = [PRICE] * 6
    stats, trades, equity, strategy, _runner = _run(
        lambda s: _BuySellBuy(s, close_at=10_000, reopen_at=10_000), closes)

    assert len(trades["pnl"]) == 0
    assert len(equity["equity"]) == 0
    assert stats["total_trades"] == 0
    assert stats["total_fees"] == 0.0
    assert stats["net_pnl"] == 0.0
    assert abs(strategy.position_at_stop - QTY) < 1e-12
