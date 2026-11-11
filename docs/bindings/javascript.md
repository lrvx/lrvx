# JavaScript Bindings (QuickJS)

lrvx embeds [QuickJS](https://bellard.org/quickjs/) to run trading strategies written in JavaScript.
Strategies use string symbol names, options objects for orders, and get TypeScript declarations for IDE autocompletion.

## Building

```bash
cmake -B build \
  -DLRVX_BUILD_CAPI=ON \
  -DLRVX_BUILD_QUICKJS=ON \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build
```

This produces:

- `build/src/quickjs/lrvx_js_runner` -- CLI to load and run JS strategies
- `build/src/quickjs/liblrvx_quickjs.a` -- static library for embedding

## Writing a Strategy

Create a `.js` file. Extend the `Strategy` base class, override `onTrade`, and call `lrvx.register`:

```javascript
class MyStrategy extends Strategy {
    constructor() {
        super({ exchange: "Binance", symbols: ["BTCUSDT"] });
    }

    onTrade(ctx, trade) {
        // ctx.symbol === "BTCUSDT"
        // trade.price, trade.qty, trade.side ("buy"/"sell")
        if (trade.price > 50000) {
            this.marketBuy({ qty: 0.1 });
        }
    }

    onStart() { console.log("Started on", this.primarySymbol); }
    onStop()  { console.log("Stopped"); }
}

lrvx.register(new MyStrategy());
```

Run it:

```bash
./build/src/quickjs/lrvx_js_runner my_strategy.js
```

## Symbol Configuration

Specify exchange and symbols in the constructor:

```javascript
// Single exchange (most common):
super({ exchange: "Binance", symbols: ["BTCUSDT", "ETHUSDT"] })

// Multi-exchange (qualified format):
super({ symbols: ["Binance:BTCUSDT", "Bybit:ETHUSDT"] })
```

Methods default to the primary symbol (first in the list) when symbol is omitted.

## Order Methods

Order methods accept an options object. Symbol defaults to primary when omitted.

```javascript
// Market
this.marketBuy({ qty: 1.0 })
this.marketSell({ symbol: "ETHUSDT", qty: 2.0 })

// Limit (tif defaults to "GTC")
this.limitBuy({ price: 50000, qty: 0.1 })
this.limitSell({ price: 51000, qty: 0.1, tif: "IOC" })

// Stop
this.stopMarket({ side: "sell", trigger: 48000, qty: 0.1 })
this.stopLimit({ side: "buy", trigger: 52000, price: 52100, qty: 0.1 })

// Take profit
this.takeProfitMarket({ side: "sell", trigger: 55000, qty: 0.1 })
this.takeProfitLimit({ side: "sell", trigger: 55000, price: 54900, qty: 0.1 })

// Trailing stop
this.trailingStop({ side: "sell", offset: 100, qty: 0.1 })
this.trailingStopPercent({ side: "sell", callbackBps: 50, qty: 0.1 })

// Order management
this.cancel(orderId)
this.cancelAll()
this.modify(orderId, { price: 50100, qty: 0.2 })
this.closePosition()
```

## Context Queries

```javascript
this.position()             // current position (primary symbol)
this.position("ETHUSDT")    // specific symbol
this.bestBid()              // number, or null when there is no bid
this.bestAsk()              // number, or null when there is no ask
this.midPrice()             // number, or null when either side is empty
this.lastPrice()
this.orderStatus(orderId)
this.hasPosition             // boolean
this.primarySymbol           // "BTCUSDT"
this.symbols                 // ["BTCUSDT", "ETHUSDT"]
```

## Callback Data

`onTrade(ctx, trade)` receives:

```javascript
ctx.symbol        // "BTCUSDT" (string)
ctx.symbolId      // 1 (numeric, for advanced use)
ctx.position      // current position size
ctx.avgEntryPrice // average entry price, NaN when the position manager reports none
ctx.book.bidPrice // best bid
ctx.book.askPrice // best ask

trade.symbol      // "BTCUSDT"
trade.price       // 50123.45
trade.qty         // 1.5
trade.side        // "buy" or "sell"
trade.isBuy       // true/false
trade.timestampNs // nanosecond timestamp, a BigInt
```

Nanosecond timestamps are `BigInt`, not `Number`. A clock reading in
nanoseconds sits far past the 2^53 where a double still holds every
integer, and rounding it there gave two events 100 ns apart the same
value. Durations and order ids are still numbers. See
[the JavaScript value boundary](../explanation/javascript-value-boundary.md)
for the full list and what a strategy written against the old types has to
change.

## Indicators

Each indicator supports `.update()` for per-tick and `static compute()` for batch. A per-tick instance holds a C handle to the C++ streaming indicator, so `.update()` is one kernel step whatever the history; `.destroy()` frees the handle early, otherwise the finalizer does (see [QuickJS indicators](../reference/quickjs/indicators.md)):

| Single-value input | OHLC input | Volume input |
|---|---|---|
| SMA, EMA, RMA, DEMA, TEMA, KAMA, RSI, Slope | ATR, ADX, Stochastic, CCI, CHOP | OBV, VWAP, CVD |
| MACD, Bollinger | | |

```javascript
// Per-tick
var atr = new ATR(14);
atr.update(high, low, close);

var macd = new MACD(12, 26, 9);
macd.update(price);
console.log(macd.line, macd.signal, macd.histogram);

// Batch
var adxResult = ADX.compute(highArr, lowArr, closeArr, 14);
// adxResult.adx, adxResult.plusDi, adxResult.minusDi
```

## Order Book

```javascript
var book = new OrderBook(0.01);  // tick size
book.applySnapshot([50000, 49999], [1.5, 2.0], [50001, 50002], [0.5, 1.0]);
book.bestBid();   // 50000
book.bestAsk();   // 50001
book.mid();       // 50000.5
book.spread();    // 1.0
book.getBids(5);  // [[50000, 1.5], [49999, 2.0]]
book.getAsks(5);  // [[50001, 0.5], [50002, 1.0]]

// L3 (order-level)
var l3 = new L3Book();
l3.addOrder(1, 50000, 1.5, "buy");
l3.addOrder(2, 50001, 0.5, "sell");
l3.bestBid();  // 50000
l3.removeOrder(1);
```

## Backtesting

The QuickJS binding exposes the bare `SimulatedExecutor` — flat fee, no funding, no liquidation, no rate limits. Good for prototyping and indicator checks.

```javascript
var executor = new SimulatedExecutor();
executor.submitOrder(1, "buy", 50000, 1.0, 0, 1);  // id, side, price, qty, type, symbol
executor.onBar(1, 50100);   // symbol, close price
executor.advanceClock(ts);
executor.fillCount;         // number of fills
```

For the realistic venue stack (cross-margin Account, MM tier ladder + ADL, VIP fee schedule, funding, rate limits) drive the same strategy file from Node.js via [`lrvx.VenueStack.binanceUmFutures(...)`](../how-to/realistic-backtest.md) — the QuickJS strategy class is unchanged. Paper trading and live (ccxt.pro) wrap the same executor; see [Paper trading](../how-to/paper-trading.md) and [Connect lrvx to a CCXT exchange](../how-to/ccxt-adapter.md).

One strategy file runs backtest, paper, and live.

## Position Tracking

```javascript
var tracker = new PositionTracker();
tracker.onFill(1, "buy", 50000, 1.0);
tracker.onFill(1, "sell", 50100, 1.0);
tracker.position(1);          // 0
tracker.realizedPnl(1);       // 100
tracker.totalRealizedPnl();   // 100

// Group tracking
var groups = new PositionGroupTracker();
var pid = groups.openPosition(1, 1, "buy", 50000, 1.0);
groups.closePosition(pid, 50500);
groups.totalRealizedPnl();  // 500
```

## Profiling

```javascript
// Volume Profile
var vp = new VolumeProfile(0.01);
vp.addTrade(50000, 1.0, true);
vp.poc();            // point of control
vp.valueAreaHigh();
vp.valueAreaLow();

// Market Profile
var mp = new MarketProfile(0.01, 30, 0);
mp.addTrade(Date.now() * 1e6, 50000, 1.0, true);
mp.poc();
mp.initialBalanceHigh();
mp.isPoorHigh();

// Footprint
var fp = new FootprintBar(0.01);
fp.addTrade(50000, 1.0, true);
fp.totalDelta();
fp.totalVolume();
```

## Statistics

```javascript
lrvx.correlation([1,2,3], [1,2,3]);      // 1.0
lrvx.profitFactor([100, -50, 200, -30]);  // gross_profit / gross_loss
lrvx.winRate([100, -50, 200, -30]);       // 0.5

lrvx.bootstrapCI([1,2,3,4,5], 0.95, 10000);
// { lower: ..., median: ..., upper: ... }

lrvx.permutationTest([1,2,3], [4,5,6], 10000);  // p-value
```

## Segment Operations

```javascript
lrvx.validateSegment("/path/to/segment.flx");
lrvx.mergeSegments("/path/to/input_dir", "/path/to/output_dir");
```

## IDE Support

Copy `quickjs/types/lrvx.d.ts` and `quickjs/jsconfig.json` into your project directory for VS Code autocompletion.

## Error Handling

JS exceptions in callbacks are caught, logged to stderr, and the strategy continues.
Invalid symbol names or side values throw immediately with descriptive errors.

## Memory Limit

The JS runtime defaults to 32MB per strategy. When embedding via C++, pass a custom limit to the `LrvxJsEngine` constructor:

```cpp
LrvxJsEngine engine(64 * 1024 * 1024);  // 64MB
LrvxJsEngine engine(0);                  // unlimited
```

## Threads

A `LrvxJsStrategy` is single-threaded by construction. It owns one QuickJS
runtime, and that runtime may only be entered from the thread that created
it. `lrvx_js_runner` and every test in this repo already work that way (one
thread, one call at a time), so most embeddings never have to think about
this at all.

A live engine is different. `lrvx_live_engine_add_strategy` subscribes a
strategy to three independently-threaded event buses (trades, book
updates, bars), each with its own consumer thread, and calls straight into
the strategy's callbacks from whichever thread that is. Hand a bare
`LrvxJsStrategy`'s callbacks to a live engine and up to three threads can
enter the same runtime at once. QuickJS was not built for that, and it
does not fail safely when it happens.

For any embedding that dispatches events from more than one thread, build
a `LrvxJsExecutor` instead of a `LrvxJsStrategy`:

```cpp
#include "js_executor.h"

lrvx::SymbolRegistry registry;
lrvx::LrvxJsExecutor executor("strategy.js", registry);

auto strategy = lrvx_strategy_create(
    1, executor.symbolIds().data(), executor.symbolIds().size(),
    registryHandle, executor.getCallbacks());
executor.injectHandle(strategy);

lrvx_live_engine_add_strategy(engine, strategy, onSignal, userData);
```

`LrvxJsExecutor` builds the runtime on one dedicated thread and keeps it
there for the object's whole lifetime. The callbacks it hands back only
ever enqueue an event onto a bounded queue: the JS side of every callback
(market data, fills, order updates, start, stop, a hot-reload's callback
swap) always runs on that one thread. A full queue blocks the calling
thread rather than dropping the event. Default capacity is 65,536,
configurable through the constructor.

If an embedder reaches into the runtime from a second thread anyway (say,
by calling into a `LrvxJsStrategy` it obtained some other way), it gets a
loud failure in a debug build, or a refused and logged call in a release
build. Not memory corruption.

## Limitations

- QuickJS is an interpreter — suitable for prototyping and backtesting, latency-sensitive production should use Codon or C++
- Scripts are evaluated with `JS_EVAL_TYPE_GLOBAL`; ES module syntax (`import` / `export`) is not supported
- A strategy subscribed to more than one live event bus for the same
  symbol (the normal live-engine setup) can still race inside the core
  strategy/book-keeping state that feeds the callback, separately from the
  JS threading model above. That is being tracked on its own.
- A strategy subscribed to more than one live event bus for the same
  symbol (the normal live-engine configuration) can race inside the core
  strategy/book-keeping state that feeds the callback, independent of the
  JS layer described above. This is being tracked separately from the JS
  threading model.
