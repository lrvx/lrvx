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
| `tickSize(symbol)` | `number` | The price tick the entry holds now |
| `tickSizeDeclared(symbol)` | `boolean` | Whether order prices on the symbol are checked against it |
| `lotSize(symbol)` | `number \| null` | The quantity step the entry holds now, `null` when it declares none |

The three getters take a `Symbol` or its numeric id and read the registry entry: `tickSize(symbol)` is 0.01 with `tickSizeDeclared(symbol)` `false` for a symbol registered without a tick, and both follow a tick or lot applied to the entry later. An id that is not registered throws a `RangeError`.

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
| `orderClosed(orderId)` | Tell the runner an order it sent to the executor is finished. `orderId` is a `number` or a `bigint` |

With an executor attached the runner keeps every order that can rest, by id, so that `emit.modify` reaches `executor.replace` as that order with its price and quantity changed. Nothing tells the runner of a fill, so call `orderClosed(orderId)` from wherever the executor learns an order has reached a terminal state (filled, cancelled by the venue, rejected, expired); the entry is dropped, and an id the runner does not hold is ignored. `emit.cancel` drops the entry by itself, and market, IOC and FOK orders are never kept. The two legs of an OCO leave the record together: a cancel of either leg, or `orderClosed` with either leg's id, drops both, because the venue cancels the other leg when one fills or is cancelled.

An `emit.modify` that names an id the runner does not hold (never sent, not an order that rests, cancelled, or reported closed) is not sent to the executor, and after it `lrvx.lastError()` reads `modify: order <id> is not on record with this executor ... [unknown_order]`.

#### Reading why a signal went nowhere

Every `emit.*` function clears `lrvx.lastError()` on entry. Read it right after the emit, inside the callback that emitted: `''` means nothing refused the signal, otherwise it says what did.

| `lrvx.lastError()` ends in | What happened |
|----------------------------|---------------|
| `[kill_switch]` | `killSwitch.check` returned `false` |
| `[order_validator]` | `orderValidator.validate` returned `false` |
| `[risk_manager]` | `riskManager.allow` returned `false` |
| `[unknown_order]` | an `emit.modify` named an order the runner's executor record does not hold |

A dropped signal reads `emit: <type> signal for order <id> on symbol <n> was dropped by the <gate>; <outcome> [<token>]`. The gates are asked in the order kill switch, order validator, risk manager, and the first that says no is the one named. The emit still returns the order id it assigned.

What is put to the gates depends on what runs the strategy:

| | `Runner` and `LiveEngine` | `BacktestRunner` |
|---|---|---|
| Signals asked | signals that create an order (market, limit, stop and take-profit, trailing stop, an OCO as one signal) and the liquidity pair | signals that create an order: market, limit, stop and take-profit (market and limit), trailing stop, both legs of an OCO |
| Pass unasked | cancel, cancel-all, modify, and any reduce-only order (a `close_position`): the same rule as the backtest runner, so a triggered kill switch cannot hold back a cancel | cancel, cancel-all, modify, and any reduce-only order (a `close_position`), so a tightened limit cannot hold a strategy in a position |
| A drop skips | `onSignal`, the executor and the PnL / storage observers | the simulator or the attached executor |
| `<outcome>` | `it reached neither on_signal nor the executor` | `it reached no executor` |
| Kill switch | asked again for every signal it is asked about | stays triggered for the rest of the run once it said no |

### Reporting a live order's updates and fills

`runner.onOrderEvent(event)` reports one order event a venue sent to the strategy that issued the
order, through the callbacks a backtest uses: `PARTIALLY_FILLED` and `FILLED` reach `onFill` and
only `onFill`, every other status reaches `onOrderUpdate`.

```js
runner.onOrderEvent({
  orderId,                 // number or bigint, as emit.limitBuy returned it
  symbol,                  // the Symbol from addSymbol, or its numeric id
  side: 'buy',             // 'buy' | 'sell'
  status: 'PARTIALLY_FILLED',
  orderType: 'limit',      // optional, 'limit' when omitted
  fillQty: 0.5,            // this fill's own quantity and price,
  fillPrice: 100.0,        // not the order's running totals
  tsNs: 1700000000000000000n,
  isMaker: true,
});
```

Each strategy has a position of its own, as in a backtest: the fill counts in the position of the
strategy that issued the order (its id is the high 32 bits of the order id) before `onFill` runs,
so `ctx.position` inside `onFill` counts it, and another strategy on the same runner still reads its
own. An order id no attached strategy issued goes to every strategy subscribed to the symbol and
counts in no position.

On a sync runner the callbacks run inside the call. On a threaded runner the event is queued behind
the trades, books and bars published before it and the callbacks run later on the JS thread, like
every other callback of that runner; `onOrderEvent` throws unless the runner is started. A missing
or mistyped field, or an unknown side, status or order type, throws a `TypeError` that names the
value.

The status names are the `OrderEventStatus` names of the engine: `NEW`, `SUBMITTED`, `ACCEPTED`,
`PARTIALLY_FILLED`, `FILLED`, `PENDING_CANCEL`, `CANCELED`, `EXPIRED`, `REJECTED`, `REPLACED`,
`PENDING_TRIGGER`, `TRIGGERED`, `TRAILING_UPDATED`, `QUEUE_POSITION_UPDATED`,
`MARKET_POSITION_CHANGED`, `REPLACE_SUBMITTED`, `REPLACE_ACCEPTED`, `REPLACE_REJECTED`,
`REJECTED_RATE_LIMIT`, `PENDING_ONCHAIN`, `REVERTED`, `REPLACED_GAS`. Node and QuickJS used to read a
`SUBMITTED` event as `"ACCEPTED"`, an `ACCEPTED` event as `"PENDING_NEW"` and
`REJECTED_RATE_LIMIT` as `"UNKNOWN"`; Python read the last as `"UNKNOWN"` too. All three now read
the one table.

### A position the account already holds

`runner.seedPosition(strategyIndex, symbol, position, avgEntryPrice?)` sets what the strategy at
`strategyIndex` (the order of `addStrategy`, from 0) holds before any fill is reported: after a
restart the account may not be flat, and no `onOrderEvent` will tell of that position. `position`
is signed, negative for a short. The strategy reads it from `ctx.position` and the fills reported
afterwards add to it. A position is seeded once: a strategy that already holds one in the symbol,
or an index the runner does not hold, throws.

`onSignal` is an observer: a runner created without one gates and forwards its signals the same way.

#### Argument checks

Every `emit.*` function checks its arguments before it emits and throws a `TypeError` that names the call, such as `emit.cancel(orderId) expected: orderId is a non-negative integer Number or a BigInt`:

- a price, quantity or liquidity must be a `number`;
- an order id (`emit.cancel`, `emit.modify`) must be a non-negative integer `number` or a `bigint`. `emit.cancel` used to read it unchecked: `NaN`, a negative or a fractional number was converted and a cancel for whatever id came out was emitted, and a `bigint` threw a bare `A number was expected`;
- the optional symbol must be a `Symbol` or a symbol id; left out or `undefined`, it is the strategy's first symbol.

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
| `traceOrderEvent(opts)` | Mirror an order event into the attached recorder. No-op with no recorder attached. `orderId` and `parentSignalId` are a non-negative integer Number or a BigInt |
| `traceFill(opts)` | Mirror a fill into the attached recorder. `orderId` and `fillId` as above |

Arguments are checked before anything is done with them. A symbol that is not a `Symbol` or a
whole symbol id, a price or quantity that is not a number, an `isBuy` that is not a boolean, an id
that is NaN, fractional, negative or a string: each is a `TypeError` naming the argument
(`onTrade: price must be a number`, `traceFill: fillId must be a non-negative integer Number (at
most 2^53) or a BigInt below 2^64`). The same holds for `emit.lastClosedBar`,
`emit.lastNClosedBars` and `emit.setBarRingCapacity` inside a strategy. These reads used to be
unchecked: the call threw a bare `A number was expected`, or went ahead with whatever the
conversion produced.

The same rule holds across the Node module. A clock reading or a count is a finite Number
(truncated toward zero) or a BigInt; a BigInt that does not fit 64 signed bits is a `RangeError`
(it used to wrap), and anything else is a `TypeError` naming the argument. `addSymbol` refuses an
exchange or symbol that is not a string and a tick or lot that is not a number (a non-number tick
was taken as "no tick"). The `BacktestRunner`, `Runner` and `WalkForwardRunner` constructors
refuse a registry that is not a `SymbolRegistry` with a `TypeError` (a plain object threw a bare
`Invalid argument`, and an instance of another class was read as a registry). A typed array of
the wrong element type is a `TypeError`, and these errors keep their class when a method turns
native errors into JS ones.

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
