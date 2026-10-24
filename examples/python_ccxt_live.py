"""Live SMA crossover on Binance public WebSocket via CcxtBroker.

The broker owns the SymbolRegistry, the Runner, and the ccxt.pro
exchange. ``self.market_buy(...)`` inside the strategy is what
actually submits the order to the exchange — there is no separate
signal handler the user has to wire up.

No API key is needed for the public trade / book streams. Order
placement requires keys; uncomment the ``api_key`` / ``secret`` /
``sandbox=True`` lines and supply your testnet credentials before
removing the dry-run guard.

Usage::

    cd /path/to/lrvx
    PYTHONPATH=build/python python3 docs/examples/python_ccxt_live.py

Stop with Ctrl+C.
"""
from __future__ import annotations

import asyncio
import os

import lrvx
from lrvx.ccxt import CcxtBroker


DRY_RUN = os.environ.get("LRVX_LIVE", "0") != "1"


class SMAStrategy(lrvx.Strategy):
    def __init__(self, symbols):
        super().__init__(symbols)
        self.fast = lrvx.SMA(10)
        self.slow = lrvx.SMA(30)
        self.tick_count = 0

    def on_start(self):
        print(f"strategy started (dry_run={DRY_RUN})")

    def on_stop(self):
        print(f"strategy stopped (saw {self.tick_count} trades)")

    def on_trade(self, ctx, trade):
        self.tick_count += 1
        if self.tick_count % 50 == 0:
            print(f"  {self.tick_count} trades processed  last={trade.price}")
        f = self.fast.update(trade.price)
        s = self.slow.update(trade.price)
        if f is None or s is None or not self.slow.ready:
            return
        if DRY_RUN:
            # Don't actually submit anything in dry-run — just log the
            # crossover state so the example is safe to run on a
            # public account.
            if f > s and ctx.is_flat():
                print(f"  [dry] would buy 0.001 @ ~{trade.price}")
            elif f < s and ctx.is_long():
                print(f"  [dry] would sell 0.001 @ ~{trade.price}")
            return
        if f > s and ctx.is_flat():
            self.market_buy(0.001)
        elif f < s and ctx.is_long():
            self.market_sell(0.001)

    def on_order_update(self, sym, status, filled, avg):
        print(f"  order {sym} status={status} filled={filled} avg={avg}")

    def on_initial_position(self, sym, qty, avg):
        print(f"  starting position on {sym}: qty={qty} avg={avg}")


async def main():
    # For real order placement, set the env var LRVX_LIVE=1 and pass
    # api_key / secret / sandbox=True. Keep DRY_RUN otherwise.
    broker = CcxtBroker(
        "binance",
        api_key=os.environ.get("BINANCE_API_KEY"),
        secret=os.environ.get("BINANCE_SECRET"),
        sandbox=not DRY_RUN,
    )
    async with broker:
        btc = await broker.add_symbol("BTC/USDT")
        broker.add_strategy(SMAStrategy([btc]))
        try:
            await broker.run(streams=("trades", "orders") if not DRY_RUN else ("trades",))
        except (KeyboardInterrupt, asyncio.CancelledError):
            pass


if __name__ == "__main__":
    asyncio.run(main())
