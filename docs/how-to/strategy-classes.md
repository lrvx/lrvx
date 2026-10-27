# Strategy classes

lrvx exposes the same event-driven Strategy model in every language. Override callbacks (`on_trade`, `on_book_update`, `on_bar`), emit orders through the strategy's helper methods, and the C++ engine handles bus dispatch, fills, and bookkeeping.

## Architecture

```
                   C++ Strategy (canonical)
                            |
                    C API (liblrvx_capi.so)
              ____________ | ____________
             |             |              |
   Python Strategy   Node Strategy   Codon Strategy
       (pybind11)        (N-API)      (C FFI, AOT)
```

All bindings dispatch through the same C++ `BridgeStrategy` callbacks, so behaviour is identical across languages.

## A simple strategy

10/30 SMA crossover, long-only.

=== "Python"

    ```python
    import lrvx

    class SmaCrossover(lrvx.Strategy):
        def __init__(self, symbols, fast=10, slow=30):
            super().__init__(symbols)
            self.prices = []
            self.fast, self.slow = fast, slow

        def on_trade(self, ctx, trade):
            self.prices.append(trade.price)
            if len(self.prices) < self.slow:
                return
            f = sum(self.prices[-self.fast:]) / self.fast
            s = sum(self.prices[-self.slow:]) / self.slow
            if f > s and ctx.is_flat():
                self.market_buy(1.0)
            elif f < s and ctx.is_long():
                self.close_position()
    ```

=== "Node.js"

    ```javascript
    class SmaCrossover {
      constructor(symbols, fast = 10, slow = 30) {
        this.symbols = symbols;
        this.fast = fast; this.slow = slow;
        this.prices = [];
      }
      onTrade(ctx, trade, emit) {
        this.prices.push(trade.price);
        if (this.prices.length < this.slow) return;
        const f = this.prices.slice(-this.fast).reduce((a,b)=>a+b)/this.fast;
        const s = this.prices.slice(-this.slow).reduce((a,b)=>a+b)/this.slow;
        if (f > s && ctx.position === 0)        emit.marketBuy(1.0);
        else if (f < s && ctx.position > 0)     emit.closePosition();
      }
    }
    ```

=== "Codon"

    ```python
    from lrvx.strategy import Strategy
    from lrvx.context import SymbolContext
    from lrvx.types import TradeData
    from lrvx.indicators import SMA

    class SmaCrossover(Strategy):
        fast_sma: SMA
        slow_sma: SMA

        def __init__(self, symbols: List[int], fast: int = 10, slow: int = 30):
            super().__init__(symbols)
            self.fast_sma = SMA(fast)
            self.slow_sma = SMA(slow)

        def on_trade(self, ctx: SymbolContext, trade: TradeData):
            price = trade.price.to_double()
            f = self.fast_sma.update(price)
            s = self.slow_sma.update(price)
            if not self.slow_sma.ready:
                return
            if f > s and self.position() == 0.0:
                self.market_buy(1.0)
            elif f < s and self.position() > 0.0:
                self.close_position()
    ```

=== "C++"

    ```cpp
    class SmaCrossover : public lrvx::Strategy {
      // ... see docs/how-to/backtest.md for the full C++ class
    };
    ```

## Overridable callbacks

| Callback | When it fires | Available |
|---|---|---|
| `on_trade(ctx, trade)` | Tick-level trade event | All bindings |
| `on_book_update(ctx)` | L2 book changed (bid/ask, mid) | All bindings |
| `on_bar(ctx, bar)` | Closed OHLC bar | All bindings (since [#123](https://github.com/lrvx/lrvx/pull/123)) |
| `on_start()` / `on_stop()` | Lifecycle hooks | All bindings |
| `on_fill(ctx, event)` | Every fill on the strategy's own orders (`PARTIALLY_FILLED` / `FILLED`) | Python, Node.js, Codon |
| `on_order_update(ctx, event)` | Every order-lifecycle status change — `NEW` / `ACCEPTED` / `CANCELED` / `REJECTED` / `REPLACED` / `TRIGGERED` / `TRAILING_UPDATED`, fills included | Python, Node.js, Codon |
| `on_market_position_change(ctx, event)` | Resting limit order's categorical market position moved (`best` / `behind_best` / `mid_spread` / `level_empty` / `crossed`). Backtest only | Python, Node.js, Codon |
| `on_queue_position_change(ctx, event)` | Resting limit order's queue position moved with no other lifecycle transition; `event.queue_ahead` / `queue_total` carry the snapshot. Backtest only | Python, Node.js, Codon |

Node spells the last four `onFill`, `onOrderUpdate`,
`onMarketPositionChange`, `onQueuePositionChange`.

## Order emission helpers

Names differ slightly by language (snake_case vs camelCase) but the surface is the same.

| Action | Python | Node.js | Codon | C++ |
|---|---|---|---|---|
| Market buy | `market_buy(qty)` | `emit.marketBuy(qty)` | `market_buy(qty)` | `emitMarketBuy(sym, qty)` |
| Market sell | `market_sell(qty)` | `emit.marketSell(qty)` | `market_sell(qty)` | `emitMarketSell(sym, qty)` |
| Limit buy | `limit_buy(price, qty, tif="gtc")` | `emit.limitBuy(p, qty)` | `limit_buy(price, qty, tif="gtc")` | `emitLimitBuy(sym, p, qty[, tif])` |
| Limit sell | `limit_sell(price, qty, tif="gtc")` | `emit.limitSell(p, qty)` | `limit_sell(price, qty, tif="gtc")` | `emitLimitSell(sym, p, qty[, tif])` |
| Cancel | `cancel_order(id)` | `emit.cancel(id)` | `cancel_order(id)` | `emitCancel(id)` |
| Cancel all | `cancel_all_orders()` | — | `cancel_all_orders()` | `emitCancelAll(sym)` |
| Modify / replace | `modify_order(id, new_price, new_qty)` | — | `modify_order(id, new_price, new_qty)` | `emitModify(id, newPrice, newQty)` |
| Stop market | `stop_market(side, trigger, qty)` | — | `stop_market(side, trigger, qty)` | `emitStopMarket(sym, side, trigger, qty)` |
| Stop limit | `stop_limit(side, trigger, limit_price, qty)` | — | `stop_limit(side, trigger, limit_price, qty)` | `emitStopLimit(sym, side, trigger, limit, qty)` |
| Take profit market | `take_profit_market(side, trigger, qty)` | — | `take_profit_market(side, trigger, qty)` | `emitTakeProfitMarket(sym, side, trigger, qty)` |
| Take profit limit | `take_profit_limit(side, trigger, limit_price, qty)` | — | `take_profit_limit(side, trigger, limit_price, qty)` | `emitTakeProfitLimit(sym, side, trigger, limit, qty)` |
| Trailing stop | `trailing_stop(side, offset, qty)` | — | `trailing_stop(side, offset, qty)` | `emitTrailingStop(sym, side, offset, qty)` |
| Trailing stop (bps) | `trailing_stop_percent(side, callback_bps, qty)` | — | `trailing_stop_percent(side, callback_bps, qty)` | `emitTrailingStopPercent(sym, side, callbackBps, qty)` |
| Close position | `close_position()` | `emit.closePosition()` | `close_position()` | `emitClosePosition(sym)` |

In Python / Node.js / Codon `symbol` defaults to the first registered symbol when omitted.

Two exits that look alike are not. `market_sell(qty)` is a plain sell of `qty`: it reduces a long by that much and goes short past it. `close_position()` sells exactly the open position as a reduce-only order, and submits nothing when flat.

The Node `emit` object carries only `marketBuy`, `marketSell`,
`limitBuy`, `limitSell`, `cancel`, `closePosition`,
`provideLiquidity`, and `withdrawLiquidity`. `cancelAll(symbol)` is a
`SimulatedExecutor` method, not an emit helper. `tif` is not
reachable from Node's `limitBuy` / `limitSell`.

`tif` accepts `gtc` (default), `ioc`, `fok`, or `post_only`;
anything else falls back to `gtc`.

## Context (`ctx`) properties

| Property | Type | Description |
|---|---|---|
| `position` | float | Current position quantity (signed) |
| `symbol_id` (Py/Codon) / `symbolId` (Node) | int | Numeric symbol id |
| `last_trade_price` / `lastTradePrice` | float | Most recent trade price |
| `best_bid` / `bestBid`, `best_ask` / `bestAsk` | float | Top-of-book |
| `mid_price` / `midPrice` | float | `(best_bid + best_ask) / 2` |
| `unrealized_pnl` (Py) | float | Unrealized PnL on the current position, `NaN` when no entry price is available |
| `is_long()` / `is_short()` / `is_flat()` (Py/Codon) | bool | Position state |
| `book_spread()` (Py/Codon) | float | `best_ask - best_bid` |

Node's `SymbolContext` is the narrower set: `position`, `symbolId`,
`lastTradePrice`, `bestBid`, `bestAsk`, `midPrice`.

Give every C++ strategy in a process its own `SubscriberId`. Nothing checks it,
and the id does double duty: the buses route on it, and it is also the
namespace for that strategy's order ids, which number from 1 within each
strategy. Duplicate ids mean duplicate order ids, which the executor and the
order tracker cannot tell apart. The bindings assign ids for you.

Allocate a C++ `Strategy` on the heap. It holds 256 per-symbol context slots by
value and each one carries a full 512-level book, so the object comes to about
2 MB in a release build and about 4 MB in a checked one. Two in the same
function overrun a default 8 MB stack wherever scale checks are on, and the
overflow lands in the constructor prologue before a single line of the strategy
has run. The bindings allocate for you.

`unrealized_pnl` is measured against the average entry price the attached
position manager reports. A manager that keeps no cost basis reports none, and
the field comes through as `NaN` instead of a number; the JavaScript engine's
`ctx.avgEntryPrice` is `NaN` in the same case. Test for it before comparing
against a threshold. The two shipped managers, `PositionTracker` and
`MultiModePositionTracker`, report an entry price whenever a position is open.

## C++ ↔ binding name map

For the canonical reference of every callback and emit helper, see the per-language API references:

- [Python reference](../reference/python/strategy.md)
- [Node.js reference](../reference/node/strategy.md)
- [Codon reference](../reference/codon/strategy.md)
- [C++ Strategy](../reference/api/strategy/strategy.md)
