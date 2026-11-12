# Paper trading with `PaperBroker`

`PaperBroker` runs your strategy against a live market data feed with its orders filled by the backtest's simulator instead of a real exchange. Paper mode is a backtest run fed live: the broker owns an `lrvx.BacktestRunner` and pushes each observed trade through it the way a tick replay (`run_tape`) does, so routing, fills, order events, positions and the tick rule are the backtest's own. A strategy that survives backtest gets to rehearse against current market conditions before any real capital is at stake, and behaves the same while it does.

## Install

```bash
pip install lrvx
```

If you also want to drive the broker off a live ccxt feed, add `[ccxt]`:

```bash
pip install "lrvx[ccxt]"
```

The broker itself does not depend on ccxt; it is feed-agnostic.

## Quick start

```python
import lrvx
from lrvx.paper import PaperBroker

registry = lrvx.SymbolRegistry()
sym = registry.add_symbol("paper", "BTCUSDT", tick_size=0.01)

broker = PaperBroker(
    registry=registry,
    default_slippage_model="fixed_bps",
    default_slippage_params={"bps": 5.0},  # 5 bps against the taker
)

class MyStrategy(lrvx.Strategy):
    def on_trade(self, ctx, trade):
        if ctx.is_flat():
            self.market_buy(0.01)

broker.set_strategy(MyStrategy([int(sym)]))   # or broker.runner.add_strategy(...)
broker.start()

# In your feed loop, forward each trade:
for ts_ns, price, qty, is_buy in your_live_feed():
    broker.observe_trade(int(sym), price, qty, is_buy, ts_ns)

broker.stop()
print(broker.fills_list())
```

## How the wiring works

```
your feed ──► broker.observe_trade ──► runner clock ──► SimulatedExecutor ──► strategy.on_trade

strategy ──► signal ──► BacktestRunner.onSignal ──► SimulatedExecutor
                                                          │
                     position tracker ◄── order events ◄──┘
                            │
                            └──► listeners ──► strategy.on_order_update / on_fill
```

`start()` builds the runner and opens a live session on it (`BacktestRunner.begin_session`). Each `observe_trade` moves the runner's clock to the trade's `ts_ns`, lets the simulator match resting and conditional orders against it, then shows it to the strategy — the order a tick replay uses. A signal the strategy emits goes to the runner's own signal handler, so it is turned into an order and gated exactly as in a backtest. Every order event comes back through the runner's listener chain: the position tracker first, then attached listeners, then the strategy's `on_order_update` / `on_fill` / `on_queue_position_change` / `on_market_position_change`. No network call, no exchange. `stop()` ends the session and returns its stats in `run_tape`'s shape; `fills_list()` reads the simulator's fills.

Because it is the same run, a strategy fed the same trades produces the same order events (status, fields, order), the same `ctx.position` and `position()` inside each callback, and the same final position in paper mode as in `run_tape`. `python/tests/test_paper_backtest_equivalence.py` holds that against a strategy that arms a stop and a take-profit from `on_fill`, modifies and cancels a resting limit, and sends an off-tick limit.

`ts_ns` is the simulator's clock, and the clock that ack latency, time-in-force expiry and order timestamps run on: pass the venue's trade time.

## Slippage and queue model

`PaperBroker` takes the backtest runner's slippage settings. The default is no slippage. Set the model at construction (or with `set_default_slippage` before `start()`):

```python
broker = PaperBroker(
    registry=registry,
    default_slippage_model="volume_impact",
    default_slippage_params={"impact_coeff": 0.002},
)
```

Or per symbol, at any time:

```python
broker.set_symbol_slippage(int(sym), "fixed_bps", bps=10.0)
```

`fee_rate`, `initial_capital` and `runner_options` (any other `lrvx.BacktestRunner` keyword, such as `submit_ack_latency_ns`) configure the runner the same way.

## What is routed

Everything a backtest routes: market and limit orders (with their time in force and flags), stop and take-profit orders (market and limit), trailing stops (offset and percent), `close_position`, `modify_order`, `cancel_order`, `cancel_all_orders` and OCO. The registry's ticks apply: an order priced off a declared tick is rejected with `price_off_tick`, `trigger_off_tick` or `trailing_offset_off_tick`, and a modify with `REPLACE_REJECTED`, exactly as in a backtest and as the live connectors refuse it.

`on_signal`, when given, receives each `lrvx.Signal` after the runner has routed it; an exception it raises is recorded in `broker.stats.error` rather than stopping the session.

One broker runs one strategy, as a backtest does: the runner's position tracker and order ids belong to it. Run a second broker for a second strategy.

## Hooking in ccxt for a live feed

```python
import asyncio
import ccxt.pro as ccxt
from lrvx import SymbolRegistry
from lrvx.paper import PaperBroker

async def main():
    registry = SymbolRegistry()
    btc = registry.add_symbol("ccxt", "BTCUSDT", tick_size=0.01)

    broker = PaperBroker(
        registry=registry,
        default_slippage_model="fixed_bps",
        default_slippage_params={"bps": 5.0},
    )
    # ... add_strategy / start as in the quick start ...
    broker.start()

    exchange = ccxt.binance()
    try:
        while True:
            trades = await exchange.watch_trades("BTC/USDT")
            for t in trades:
                broker.observe_trade(
                    int(btc),
                    float(t["price"]),
                    float(t["amount"]),
                    t["side"] == "buy",
                    int(t["timestamp"]) * 1_000_000,
                )
    finally:
        broker.stop()
        await exchange.close()
        print(broker.fills_list())

asyncio.run(main())
```

This is the minimal pattern. Production setups will add error handling around `watch_trades`, persistent reconnection, and probably a separate task for `watch_order_book` plus `broker.observe_book_snapshot` so the simulator's queue tracker has top-of-book context.

## See also

* [Backtest with realistic fills](backtest-realistic-fills.md). The same `SimulatedExecutor` powering paper mode runs the backtest fills.
* [CCXT adapter](ccxt-adapter.md). The live order broker for the same lrvx pipeline; swap in `PaperBroker` to rehearse without sending live orders.
* [Record and replay tapes](tape-record.md). Recorded tapes can drive `PaperBroker` instead of a live feed for repeatable rehearsals.
