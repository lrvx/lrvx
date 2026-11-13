# Connect lrvx to a CCXT exchange

`lrvx.ccxt.CcxtBroker` wraps a [ccxt.pro](https://github.com/ccxt/ccxt/wiki/ccxt.pro) exchange. Market data flows in (trades, L2 books, order updates) and orders flow out (`self.market_buy(...)`, `self.limit_sell(...)`, `self.stop_market(...)` from inside the strategy translate into `create_*_order` calls on the exchange).

## Install

```bash
pip install lrvx "ccxt[pro]"
```

## Quick start

```python
import asyncio
import lrvx
from lrvx.ccxt import CcxtBroker


class SMA(lrvx.Strategy):
    def __init__(self, symbols):
        super().__init__(symbols)
        self.fast = lrvx.SMA(10)
        self.slow = lrvx.SMA(30)

    def on_trade(self, ctx, trade):
        f = self.fast.update(trade.price)
        s = self.slow.update(trade.price)
        if f is None or s is None:
            return
        if f > s and ctx.is_flat():
            self.market_buy(0.001)
        elif f < s and ctx.is_long():
            self.market_sell(0.001)

    def on_fill(self, ctx, event):
        print(f"  order {event.order_id} {event.status} {event.fill_qty} @ {event.fill_price}"
              f" position={ctx.position}")

    def on_order_update(self, ctx, event):
        print(f"  order {event.order_id} {event.status}")

    def on_initial_position(self, sym, qty, avg):
        print(f"  starting position on {sym}: qty={qty} avg={avg}")


async def main():
    async with CcxtBroker(
        "binance",
        api_key="...",       # leave None for public market data only
        secret="...",
        sandbox=True,
    ) as broker:
        btc = await broker.add_symbol("BTC/USDT")
        broker.add_strategy(SMA([btc]))
        await broker.run(streams=("trades", "book"))


asyncio.run(main())
```

Other constructor keywords: `password` (for venues that need a passphrase), `on_error` (a `Callable[[str, BaseException], None]` invoked per stream failure with a tag like `"balance"` / `"orders"`; the default logs at WARN), and `exchange` (dependency injection for tests — production callers leave it `None` and the broker constructs the ccxt.pro exchange itself).

`add_symbol` calls `exchange.load_markets()` once and reads tick size from `markets[sym]["precision"]["price"]`, resolved against `exchange.precisionMode` when the exchange exposes it, since ccxt's three precision conventions (`DECIMAL_PLACES`, `SIGNIFICANT_DIGITS`, `TICK_SIZE`) are not interchangeable. `add_strategy` attaches to the broker's internal `Runner`. The runner's signal callback is wired to `_handle_signal`, which dispatches to a per-order-type method (`_place_market`, `_place_limit`, `_place_stop_market`, ...). Calling `self.market_buy(...)` inside the strategy ends up calling `exchange.create_market_buy_order(...)`.

## Lifecycle

`async with CcxtBroker(...)` constructs the exchange. `await broker.run(...)` starts the runner, dispatches initial-position callbacks, spawns one asyncio task per (symbol × stream), and awaits until cancelled. On `__aexit__` the broker cancels stream tasks, stops the runner, and closes the exchange.

`broker.run(...)` blocks until cancelled (e.g. via `Ctrl+C` or an outer task cancel). To run alongside other coroutines, schedule it as a task and cancel it on shutdown.

## Streams

Pass any subset of `("trades", "book", "bbo", "orders")` to `run(streams=...)`:

| Stream    | ccxt method               | lrvx target                                                    |
|-----------|---------------------------|----------------------------------------------------------------|
| `trades`  | `watch_trades(sym)`       | `runner.on_trade(sym_id, price, qty, is_buy, ts_ns)`           |
| `book`    | `watch_order_book(sym)`   | `runner.on_book_snapshot(sym_id, bids/asks)`                   |
| `bbo`     | `watch_bids_asks(sym)`    | `runner.on_book_top(sym_id, bid, ask)`                         |
| `orders`  | `watch_orders()`          | `runner.on_order_event(...)`: the strategy's `on_fill` / `on_order_update` |

`orders` need not be named. The broker opens it by itself before it places its first order (and
before a modify), whatever `streams` says: a broker that places orders needs their updates, and
with the default `("trades", "book")` a resting order used to be never reported filled and never
left the broker's maps. A broker that only watches market data never opens it, so it runs without
credentials.

## Order updates and fills

Order updates reach the strategy through the callbacks a backtest and paper mode use, with the same
arguments, so one strategy class runs in all three:

| The exchange reports | Event `status` | Callback |
|----------------------|----------------|----------|
| `open`, the first time the order is seen | `ACCEPTED` | `on_order_update(ctx, event)` |
| `filled` grew and the order still works | `PARTIALLY_FILLED` | `on_fill(ctx, event)` |
| `filled` grew and the order is `closed` or has nothing remaining | `FILLED` | `on_fill(ctx, event)` |
| `canceled`, `expired`, `rejected` | `CANCELED`, `EXPIRED`, `REJECTED` | `on_order_update(ctx, event)` |

`event` is a `lrvx.OrderEventData`. `event.order_id` is the id the emit returned, `event.side` and
`event.order_type` are the order's. A fill reaches `on_fill` and only `on_fill`, and `ctx.position`
inside it already counts the fill. ccxt reports an order's running totals; `event.fill_qty` is the
quantity of this fill alone (how much `filled` grew since the order was last reported) and
`event.fill_price` its price, worked back from the change in `average`. The answer to a placement,
the answer to a cancel and the stream may all report the same state of an order: only what is new
produces an event. An order the broker did not place carries `order_id` 0 and goes to every strategy
subscribed to its symbol; each of the broker's own goes to the strategy that emitted it.

The broker used to call `strategy.on_order_update(ccxt_sym, status, filled, avg)`. That is not the
signature `lrvx.Strategy` declares, so a strategy that did not override the method raised
`TypeError` on every update, and a fill never reached `on_fill`.

**Compatibility.** A strategy whose `on_order_update` takes four arguments, the old form, still
receives exactly that call (`ccxt_sym`, ccxt's `status`, cumulative `filled`, `average`) for every
update on the orders stream, and `add_strategy` warns with a `DeprecationWarning`. Its
`on_order_update` is not called with `(ctx, event)`; its `on_fill` is called like any other
strategy's. Move to the two-argument form to receive `ACCEPTED` / `CANCELED` / `EXPIRED` /
`REJECTED` as events.

The broker keeps, for every order it placed that is still open, the exchange's
order id and what was placed, so that a later `cancel_order` or `modify_order`
can name it to the exchange. An order leaves those maps when the strategy
cancels it (a `cancel_all_orders` drops the orders whose cancel the exchange accepted and calls
`Runner.order_closed` for each; one whose cancel failed stays tracked and the failure goes to
`on_error`), when `orders` reports a terminal status for it (`closed`,
`canceled`, `expired`, `rejected`), and at once when the exchange's answer to
the placement already carries a terminal status, as a market order's usually
does. `Runner.order_closed` is called for it at the same moment. A
`cancel_order` or `modify_order` for an order that has left is not sent; it is
reported through `on_error`.

`trades` and `book` yield a snapshot per call. `bbo` yields a touch-only
frame: it lands in the strategy's touch fields without replacing the depth
book `book` maintains, so the touch stays fresh between (often throttled)
depth updates.

Book depth is a keyword on `run()`, not on the constructor: `run(book_depth=N)`. It defaults to `None`, which means no `limit` is passed to `watch_order_book` and ccxt picks the venue's default channel. The one exception is a built-in per-exchange override — `_BOOK_DEPTH_OVERRIDE = {"bitget": 15}`, because ccxt.pro's default `books50` channel for Bitget USDT-FUTURES is rejected by the venue with error 30016.

## Order routing

When a strategy emits a signal (via `self.market_buy(...)` etc.), the broker's signal callback maps it to ccxt:

| `Signal.order_type`         | ccxt call                                                                         |
|-----------------------------|-----------------------------------------------------------------------------------|
| `MARKET`                    | `create_market_<side>_order(sym, qty)`                                            |
| `LIMIT`                     | `create_limit_<side>_order(sym, qty, price)`                                      |
| `STOP_MARKET`               | `create_order(sym, "stop_market", side, qty, None, {"stopPrice": trigger})`       |
| `STOP_LIMIT`                | `create_order(sym, "stop_limit", side, qty, limit, {"stopPrice": trigger})`       |
| `TAKE_PROFIT_MARKET`        | `create_order(sym, "take_profit_market", side, qty, None, {"stopPrice": ...})`    |
| `TAKE_PROFIT_LIMIT`         | `create_order(sym, "take_profit_limit", side, qty, limit, {"stopPrice": ...})`    |
| `TRAILING_STOP`             | `create_order(sym, "trailing_stop_market", side, qty, None, {"trailingDelta": offset})` |
| `TRAILING_STOP_PERCENT`     | `create_order(sym, "trailing_stop_market", side, qty, None, {"callbackRate": bps/100})` |
| `CLOSE_POSITION`            | `create_order(sym, "market", side, qty, None, {"reduceOnly": True})`              |
| `CANCEL`                    | `cancel_order(ccxt_id, ccxt_sym)` using the id and symbol tracked from the original signal |
| `CANCEL_ALL`                | `cancel_order` for every tracked id on the symbol                                 |
| `MODIFY`                    | `edit_order(ccxt_id, ccxt_sym, type, side, new_qty, new_price)` with the symbol, side and type the order was placed with |

A `CANCEL` or `MODIFY` signal names an order id and nothing else, so the broker keeps the symbol, side and ccxt type of every order it places and sends those. The new price of a modify is the limit price; for a stop-market, take-profit-market or trailing order, which has none, it is the trigger and goes out as `edit_order(ccxt_id, ccxt_sym, type, side, new_qty, None, {"stopPrice": new_price})`. A stop-limit or take-profit-limit keeps its trigger: `{"stopPrice": <the original trigger>}` rides along with the new limit price. When the exchange answers an edit with a new order id, later cancels and modifies use it. A modify for an id the broker did not place, or one already cancelled, goes to `on_error`.

Stop, take-profit, and trailing parameter shapes differ across exchanges. The broker uses ccxt's unified-API defaults. If your exchange wants a different params dict (Binance futures uses `triggerPrice` instead of `stopPrice`, OKX uses `triggerPx`), subclass and override `_place_stop_market`, `_place_trailing`, etc.

Order types outside the table dispatch to `on_error(sym, UnsupportedOrderType(...))`. The exchange call is skipped.

## Position reconciliation

`run(reconcile=True)` (default) calls `fetch_balance()` and `fetch_positions()` before streams start. For each non-zero position on a registered symbol the broker calls `strategy.on_initial_position(ccxt_sym, qty, avg_price)`. If the strategy has no such method the position is just logged.

`fetch_positions` is a derivatives concept. Spot exchanges raise `NotSupported`; the broker swallows it and proceeds with an empty position list.

## Backoff

Stream errors (network drops, exchange 5xx, etc.) are caught and retried with exponential backoff:

```python
broker = CcxtBroker(
    "binance",
    retry_initial_delay=1.0,    # first sleep after a failure
    retry_max_delay=60.0,       # cap
    retry_multiplier=2.0,       # delay *= multiplier on each consecutive fail
)
```

Delay starts at `retry_initial_delay`, multiplies on each consecutive failure, caps at `retry_max_delay`, and resets to the initial value after a successful yield.

## Subclassing per exchange

Override any of the `_place_*` methods to customise per-exchange params (Bybit `triggerBy`, OKX `tdMode`, Binance futures `closePosition`, etc.):

```python
class BybitBroker(CcxtBroker):
    async def _place_stop_market(self, sym, side, sig):
        await self.exchange.create_order(
            sym, "stop_market", side, self._qty(sig), None,
            {"stopPrice": self._trigger(sig), "triggerBy": "MarkPrice"},
        )
```

## Recording market data

`broker.set_market_data_recorder(recorder)` attaches a `MarketDataRecorderHook` subclass that sees every trade and book update flowing through the broker. This is the hook `lrvx tape record` uses, and it is also the fix when a capture's `metadata.json` comes out with an empty `symbols` list — attach the recorder before `run()` so the symbol registrations are captured.

## Multi-exchange

Instantiate one broker per exchange. A strategy attached to one broker only sees that exchange's market data; symbol ids are scoped to the broker that registered them.

## Runnable example

A complete script lives at [`docs/examples/python_ccxt_live.py`](../examples/python-ccxt-live.md): SMA(10/30) on `BTC/USDT` against Binance's public WebSocket. Runs in dry-run mode by default; set `LRVX_LIVE=1` plus `BINANCE_API_KEY` / `BINANCE_SECRET` to actually place sandbox orders.
