"""Run a BacktestRunner against a VenueStack's fill mechanics.

The runner feeds the stack's executor market data and harvests its fills,
so the run uses that venue's queue model and depth, iceberg refresh
latency, venue availability and rate limits.

Fees come from the stack's FeeSchedule: each fill is billed at the tier
the account's 30-day notional resolves to, maker or taker by the fill's
is_maker, not at the runner's fee_rate. Funding and liquidation are driven
by explicit calls rather than the replay loop, so read the result as "this
venue's fill mechanics and fees", not "this venue's full economics".
"""
import pathlib
from collections import deque

import lrvx

CSV = str(
    pathlib.Path(lrvx.__file__).resolve().parent
    / "templates/research/data/btcusdt_sample.csv"
)


class CrossAboveSMA(lrvx.Strategy):
    def __init__(self, symbols, period=20):
        super().__init__(symbols)
        self.window = deque(maxlen=period)

    def on_trade(self, ctx, trade):
        self.window.append(trade.price)
        if len(self.window) < self.window.maxlen:
            return
        sma = sum(self.window) / len(self.window)
        if trade.price > sma and ctx.is_flat():
            self.market_buy(1.0)
        elif trade.price < sma and ctx.is_long():
            self.close_position()


def main():
    registry = lrvx.SymbolRegistry()
    btc = registry.add_symbol("binance", "BTCUSDT", tick_size=0.01)

    bt = lrvx.BacktestRunner(registry, fee_rate=0.0004, initial_capital=10_000.0)
    bt.set_strategy(CrossAboveSMA([btc]))

    stack = lrvx.VenueStack.binance_um_futures(account_id=42, equity=10_000.0)
    bt.set_venue_stack(stack)  # pass None to revert to the built-in executor

    stats = bt.run_csv(CSV, "BTCUSDT")

    print(f"trades      : {stats['total_trades']}")
    print(f"return      : {stats['return_pct']:.2f}%")
    print(f"venue fills : {len(stack.executor().fills_list())}")


if __name__ == "__main__":
    main()
