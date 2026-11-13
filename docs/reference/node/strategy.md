# Strategy, Runner

```javascript
const { SymbolRegistry, Runner } = require('@lrvx/lrvx');
```

---

## SymbolRegistry

```javascript
const registry = new lrvx.SymbolRegistry();
```

| Method | Returns | Description |
|--------|---------|-------------|
| `addSymbol(exchange, name, tickSize?, lotSize?)` | `Symbol` | Register a symbol. `tickSize` is the venue's price increment and a backtest rejects orders priced off it; left out, the book uses 0.01 and order prices are not checked. `lotSize` is the venue's quantity step and a backtest rejects an order whose quantity is not a whole number of it; left out, quantities are not checked |
| `symbolCount()` | `number` | Number of registered symbols |

### Symbol

Returned by `addSymbol`. Coerces to a number wherever a symbol ID is expected.

| Property | Type | Description |
|----------|------|-------------|
| `id` | `number` | Numeric symbol ID |
| `name` | `string` | Symbol name |
| `exchange` | `string` | Exchange name |
| `tickSize` | `number` | Tick size |
| `lotSize` | `number \| null` | Quantity step the symbol was registered with, `null` when none was given |

```javascript
const btc = registry.addSymbol('binance', 'BTCUSDT', 0.01);

btc.id         // 1
btc.name       // "BTCUSDT"
Number(btc)    // 1
btc + 0        // 1
btc.toString() // "Symbol(binance:BTCUSDT, id=1)"
```

---

## Strategy object

A plain JavaScript object with callback properties.

```javascript
const strategy = {
    symbols: [btc],   // array of Symbol or number

    onStart() {},
    onStop() {},

    onTrade(ctx, trade, emit) { ... },
    onBookUpdate(ctx, emit) { ... },
    onTouch(ctx, emit) { ... },
    onBar(ctx, bar, emit) { ... },

    onFill(ctx, ev, emit) { ... },
    onOrderUpdate(ctx, ev, emit) { ... },
    onQueuePositionChange(ctx, ev, emit) { ... },
    onMarketPositionChange(ctx, ev, emit) { ... },
};
```

All callbacks are optional.

`onTouch` fires when the touch changes (price or size on either side, or a
side appearing or emptying): on a top-of-book frame, and right after
`onBookUpdate` on a depth frame that moved it. `ctx.bestBid` / `ctx.bestAsk`
are the new touch.

### Order-event callbacks

| Callback | Fires on |
|----------|----------|
| `onFill(ctx, ev, emit)` | Every fill this strategy's own orders produce (status `PARTIALLY_FILLED` or `FILLED`) |
| `onOrderUpdate(ctx, ev, emit)` | Every order-lifecycle status change: `NEW` / `ACCEPTED` / `CANCELED` / `REJECTED` / `REPLACED` / `TRIGGERED` / `TRAILING_UPDATED` and the rest. A fill does not fire here: a `PARTIALLY_FILLED` or `FILLED` event reaches `onFill` only |
| `onQueuePositionChange(ctx, ev, emit)` | A resting limit order's queue position moved with no other lifecycle transition. `ev.queueAhead` / `ev.queueTotal` carry the snapshot. Backtest only |
| `onMarketPositionChange(ctx, ev, emit)` | A resting limit order's categorical market position transitioned (best / behind_best / mid_spread / level_empty / crossed). Backtest only |

`ctx.position` in `onFill` and `onOrderUpdate` already counts the fill being
delivered: the runner's position tracker takes a fill before the strategy
hears of it. After a buy of 1 from flat, `ctx.position` in that fill's
`onFill` is 1.

`ev` is an `OrderEventData`: `orderId`, `symbolId`, `side`, `orderType`,
`status`, `fillQty`, `fillPrice`, `exchangeTsNs`, `rejectReason`,
`queueAhead`, `queueTotal`, the per-stage timestamps `submittedAtNs` /
`acceptedAtNs` / `firstFillAtNs` / `lastFillAtNs` / `canceledAtNs` /
`rejectedAtNs` / `triggeredAtNs` / `expiredAtNs`, plus `isMaker`,
`fillRole`, `marketPosition` and `distanceToBestTicks`.

`exchangeTsNs` and the eight per-stage timestamps are `bigint`: they are
clock readings, and a JS `number` steps 256 ns at a time at today's values.
A stage the order has not reached reads `0n`. They compare against each
other and against a `number`; for a latency, subtract first and convert the
difference: `Number(ev.acceptedAtNs - ev.submittedAtNs)`.

### BarData (`bar`)

| Property | Type | Description |
|----------|------|-------------|
| `open`, `high`, `low`, `close` | `number` | OHLC prices |
| `volume`, `buyVolume` | `number` | Total / buy-side volume |
| `startTimeNs`, `endTimeNs` | `bigint` | Bar window timestamps (nanoseconds) |
| `barType`, `barTypeParam` | `number` | 0=Time, 1=Tick, ... + interval/threshold |
| `closeReason` | `number` | 0=Threshold, 1=Gap, 2=Forced, 3=Warmup. The engine emits `Threshold` for a normal close, `Gap` for the bar that absorbs the remainder of a Renko price jump too wide to walk brick by brick, and `Forced` for a `stop()` flush; `Warmup` is never assigned by the engine itself -- it is for callers who construct their own `Bar` objects from historical data before calling `BarMatrix::warmup()`. |

### SymbolContext (`ctx`)

| Property | Type | Description |
|----------|------|-------------|
| `position` | `number` | Current position quantity |
| `symbolId` | `number` | Symbol ID |
| `lastTradePrice` | `number` | Last trade price |
| `bestBid` | `number \| null` | Best bid; `null` when the bid side has no level. A book may be quoted at exactly zero, so `0` is a price, never "no quote" |
| `bestAsk` | `number \| null` | Best ask; `null` when the ask side has no level |
| `midPrice` | `number \| null` | Mid price; `null` unless both sides have a level |
| `lastUpdateNs` | `bigint` | Exchange timestamp (ns) of the last dispatched event: trade, depth frame or bar |
| `lastTouchNs` | `bigint` | Exchange timestamp (ns) of the frame that last wrote the touch. A `TOP_OF_BOOK` frame moves it without firing `onBookUpdate`, so it can be fresher than `lastUpdateNs` |

### TradeData (`trade`)

| Property | Type | Description |
|----------|------|-------------|
| `price` | `number` | Trade price |
| `qty` | `number` | Trade quantity |
| `isBuy` | `boolean` | Buy-side aggressor |
| `side` | `string` | `"buy"` or `"sell"` |
| `timestampNs` | `BigInt` | Timestamp (nanoseconds) |

### emit methods

| Method | Returns | Description |
|--------|---------|-------------|
| `emit.marketBuy(qty)` | `number` | Market buy |
| `emit.marketSell(qty)` | `number` | Market sell |
| `emit.limitBuy(price, qty)` | `number` | Limit buy |
| `emit.limitSell(price, qty)` | `number` | Limit sell |
| `emit.provideLiquidity(priceLower, priceUpper, liquidity)` | `number` | Provide AMM liquidity in a price range |
| `emit.withdrawLiquidity(liquidity)` | `number` | Withdraw AMM liquidity |
| `emit.cancel(orderId)` | `undefined` | Cancel order |
| `emit.modify(orderId, newPrice, newQty)` | `undefined` | Change the price and quantity of a working order. It keeps its id, symbol, side and type; `newQty` is the quantity left to work. `newPrice` is the trigger of a stop-market, take-profit-market or trailing-stop order and the limit price of every other type. `orderId` is the id the emitting function returned, which is also the `ev.orderId` of the order's events. A refused modify arrives in `onOrderUpdate` as `REPLACE_REJECTED` with `rejectReason` set |
| `emit.closePosition()` | `number` | Close position (reduce-only). `0` when flat: no order is sent |

The functions that send an order return its id, as the Python, Codon and
QuickJS ones do: the `orderId` the order's events carry, and what
`emit.cancel` and `emit.modify` take.

```javascript
onBar(ctx, bar, emit) {
  if (!this.orderId) this.orderId = emit.limitBuy(bar.close - 1, 1);
  else emit.modify(this.orderId, bar.close - 0.5, 1);
}
```

In a backtest, a `limitBuy` / `limitSell` price that is not a whole number of the symbol's `tickSize` is rejected: `onOrderUpdate` receives `status` `"REJECTED"` with `rejectReason` `"price_off_tick"`. See [SimulatedExecutor](../api/backtest/simulated_executor.md#order-prices-and-the-tick).

A quantity is checked when the symbol was registered with a quantity step, the fourth argument of `registry.addSymbol(exchange, name, tickSize, lotSize)`: an order whose quantity is not a whole number of `lotSize` is rejected with `rejectReason` `"quantity_off_lot"`, and an `emit.modify` to such a quantity gets `"REPLACE_REJECTED"`. Without `lotSize` quantities are not checked. The returned symbol carries it as `lotSize` (`null` when not given); a `lotSize` that is not positive throws a `RangeError`. See [SimulatedExecutor](../api/backtest/simulated_executor.md#order-quantities-and-the-lot).

---

## Runner

Synchronous strategy host. Strategy callbacks fire in the caller's thread before the push call returns.

```javascript
const runner = new lrvx.Runner(registry, onSignal);        // synchronous
const runner = new lrvx.Runner(registry, onSignal, true);  // Disruptor background thread
```

In threaded mode, events are published to a lock-free ring buffer and callbacks fire in a background C++ thread.

| Method | Description |
|--------|-------------|
| `addStrategy(strategy)` | Register a strategy object. The runner gives each strategy its own subscriber id, 1, 2, ... in the order added, and order ids are `(id << 32) + n`, so two strategies on one runner never issue the same order id. Throws when the engine refuses the strategy |
| `replaceStrategy(index, strategy)` | Atomically swap the strategy at `index`. The old strategy's `onStop` fires before the swap, the new one's `onStart` after; bus subscriptions, in-flight orders and connections are untouched. Must be invoked on the V8 thread |
| `start()` | Start the runner |
| `stop()` | Stop and clean up |
| `onTrade(symbol, price, qty, isBuy, tsNs)` | Inject a trade tick |
| `onBookSnapshot(symbol, bidPrices, bidQtys, askPrices, askQtys, tsNs)` | Inject an L2 snapshot |
| `onBookTop(symbol, bid, ask, tsNs)` | Inject a touch-only frame: moves the strategy's touch without a book callback. `bid` / `ask` are `{ price, qty }`; `null` means that side of the book is empty |
| `onBar(symbol, { open, high, low, close, volume?, ... })` | Inject a closed OHLC bar |

`symbol` accepts a `Symbol` object or a raw number. `tsNs` accepts a number or a `bigint`.
`bidPrices` / `bidQtys` / `askPrices` / `askQtys` are `Float64Array`, one entry per book
level; each price array must be the same length as its matching quantity array.

Exceptions behave differently in the two modes. In sync mode, a throw from
`onTrade` / `onBookSnapshot` / `onBar` propagates straight out of the call,
so a `try`/`catch` around it works normally. In threaded mode there's no
caller waiting on the call (it runs on a libuv-scheduled turn), so Node
just logs a one-time `DEP0168` deprecation warning and drops the
exception; the strategy quietly misses that event. Put the `try`/`catch`
inside the callback itself if a threaded strategy needs to see its own
exceptions.

### Hook setters

Each takes a plain JS object, or `null` to detach.

| Method | Description |
|--------|-------------|
| `setPnlTracker(tracker)` | Attach a PnL tracker |
| `setStorageSink(sink)` | Attach a signal storage sink |
| `setRiskManager(rm)` | Attach a pre-trade risk manager. Sync only — `allow` is read inline; throws when `threaded` |
| `setKillSwitch(ks)` | Attach a kill switch. Sync only |
| `setOrderValidator(ov)` | Attach an order validator. Sync only |
| `setMarketDataRecorder(recorder)` | Attach a `MarketDataRecorderHook` or `BinaryLogRecorderHook` |
| `setExecutor(executor)` | Replace the executor. Sync only — `capabilities()` is read inline |

#### What a hook is handed

An order — the argument to `Executor.submit` and to every
`ExecutionListener` callback — carries `createdAtNs` and `exchangeTsNs` as
`bigint`. They are clock readings, and a JS `Number` is a double that steps
256 ns at present-day magnitudes. Prices, sizes and ids on the same object
stay `number`.

`Executor.replace(oldId, order)` is called for a strategy's `emit.modify`.
`order` is the order that modify named, as the executor was handed it --
same id, symbol, side and type -- with the new price and quantity. A modify
of an id the runner never handed to this executor (or has since seen
cancelled) still arrives, under that id, with `symbol` 0.

A trade handed to a `MarketDataRecorderHook`'s `onTrade` is a `TradeData`:
`{ symbol, price, qty, isBuy, side, timestampNs }`, with `timestampNs` a
`bigint`. It is the same shape a strategy's `onTrade` receives.

`onBookUpdate(symbol, isSnapshot, bids, asks, timestampNs)` receives
snapshots and deltas. A top-of-book frame goes to
`onBookTop(symbol, bid, ask, timestampNs)` instead, with `bid` / `ask` a
`{ price, qty }` or `null` for an empty side, the shape `Runner.onBookTop`
takes. Defining `onBookTop` does not change `onBookUpdate`, which keeps
receiving every snapshot and delta. A hook without `onBookTop` skips tops.

!!! warning "Deprecated: `trade.quantity` and `trade.exchangeTsNs`"

    The recorder hook used to be handed `quantity` and `exchangeTsNs` in
    place of the declared `qty` and `timestampNs`. Both spellings are
    delivered for one release and the old two are removed in the next.
    `exchangeTsNs` is a `bigint` like `timestampNs` — the reading never fit
    in a double — so a hook doing arithmetic on it needs `Number()` at the
    edge, or the one-word change to `timestampNs`.

### Trace recording

| Method | Description |
|--------|-------------|
| `attachTraceRecorder(recorder)` | Auto-capture every signal into a `.lrvxrun` recorder. Sync mode only; throws otherwise |
| `setTraceFeedTsNs(feedTsNs)` | Stamp every recorded signal with this `feed_ts_ns` until the next call |
| `traceOrderEvent(opts)` | Mirror an order event into the attached recorder. No-op with no recorder attached |
| `traceFill(opts)` | Mirror a fill into the attached recorder |

### Signal callback

```javascript
function onSignal(sig) {
    // sig.side       — "buy" | "sell"
    // sig.quantity
    // sig.price      — 0 for market orders
    // sig.orderType  — "market" | "limit" | "stop_market" | ...
    // sig.orderId
}
```
