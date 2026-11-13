# Strategy

Extend `Strategy` and call `lrvx.register` with an instance.

```javascript
class MyStrategy extends Strategy {
    constructor() {
        super({ exchange: 'Binance', symbols: ['BTCUSDT'] });
    }

    onTrade(ctx, trade) { ... }
    onBookUpdate(ctx, book) { ... }
    onTouch(ctx, book) { ... }
    onBar(ctx, bar) { ... }
    onFill(ctx, ev) { ... }
    onOrderUpdate(ctx, ev) { ... }
    onStart() { ... }
    onStop() { ... }
}

lrvx.register(new MyStrategy());
```

### Constructor options

```javascript
// Single exchange
super({ exchange: 'Binance', symbols: ['BTCUSDT', 'ETHUSDT'] })

// Multi-exchange (qualified names)
super({ symbols: ['Binance:BTCUSDT', 'Bybit:ETHUSDT'] })
```

### Properties

| Property | Type | Description |
|---|---|---|
| `primarySymbol` | `string` | First symbol name |
| `symbols` | `string[]` | All registered symbol names |
| `hasPosition` | `boolean` | Whether a position is open (primary symbol) |

---

## Callbacks

### `onTrade(ctx, trade)`

| Field | Type | Description |
|---|---|---|
| `ctx.symbol` | `string` | Symbol name |
| `ctx.symbolId` | `number` | Numeric symbol ID |
| `ctx.position` | `number` | Current position size. In `onFill` and `onOrderUpdate` it already counts the fill being delivered, as `this.position()` does |
| `ctx.avgEntryPrice` | `number` | Average entry price, `NaN` when the position manager reports none |
| `ctx.lastTradePrice` | `number` | Price of the symbol's last trade |
| `ctx.lastUpdateNs`, `ctx.lastTouchNs` | `bigint` | Stamp of the last frame that updated the book, and of the last one that moved the touch |
| `ctx.book.bidPrice` | `number \| null` | Best bid; `null` when the bid side has no level (`0` is a price) |
| `ctx.book.askPrice` | `number \| null` | Best ask; `null` when the ask side has no level |
| `ctx.book.midPrice` | `number \| null` | Mid price; `null` unless both sides have a level |
| `ctx.book.spread` | `number \| null` | Spread; `null` unless both sides have a level |
| `trade.symbol` | `string` | Symbol name |
| `trade.symbolId` | `number` | Numeric symbol ID |
| `trade.price` | `number` | Trade price |
| `trade.qty` | `number` | Trade quantity |
| `trade.side` | `string` | `"buy"` or `"sell"` |
| `trade.isBuy` | `boolean` | |
| `trade.timestampNs` | `bigint` | Exchange timestamp, nanoseconds |

Clock readings are `BigInt` in every callback: a nanosecond epoch does not fit a
`Number`. See [the JavaScript value boundary](../../explanation/javascript-value-boundary.md).

### `onTouch(ctx, book)`

Fires when the touch changes (price or size on either side, or a side
appearing or emptying): on a top-of-book frame, and right after `onBookUpdate`
on a depth frame that moved it. `book.snapshot` and `ctx.book` are the new
touch; `book.timestampNs` is the stamp of the frame that moved it.

### `onBar(ctx, bar)`

| Field | Type | Description |
|---|---|---|
| `bar.open`, `bar.high`, `bar.low`, `bar.close` | `number` | OHLC prices |
| `bar.volume`, `bar.buyVolume` | `number` | Total / buy-side volume |
| `bar.symbol`, `bar.symbolId` | `string`, `number` | Symbol name and numeric ID |
| `bar.startTimeNs`, `bar.endTimeNs` | `bigint` | Bar window timestamps (nanoseconds) |
| `bar.barType`, `bar.barTypeParam` | `number` | 0=Time, 1=Tick, ... + interval/threshold |
| `bar.closeReason` | `number` | 0=Threshold, 1=Gap, 2=Forced, 3=Warmup. The engine emits `Threshold` for a normal close, `Gap` for the bar that absorbs the remainder of a Renko price jump too wide to walk brick by brick, and `Forced` for a `stop()` flush; `Warmup` is never assigned by the engine itself -- it is for callers who construct their own `Bar` objects from historical data before calling `BarMatrix::warmup()`. |

### `onFill(ctx, ev)` / `onOrderUpdate(ctx, ev)`

`onFill` fires for every fill of this strategy's own orders (`ev.status` is
`PARTIALLY_FILLED` or `FILLED`). `onOrderUpdate` fires for every other change of
an order's status: `ACCEPTED`, `CANCELED`, `REJECTED`, `REPLACED`, `TRIGGERED`,
`TRAILING_UPDATED` and the rest. A fill reaches `onFill` only.
`onQueuePositionChange(ctx, ev)` and `onMarketPositionChange(ctx, ev)` take the
same `ev` when a resting limit order's queue position or its place relative to
the touch changes with no other status change (backtest only).

`ctx` is the context `onTrade` receives. `ctx.position` already counts the fill
being delivered: after a buy of 1 from flat it is 1 inside `onFill`, as
`this.position()` is.

| Field | Type | Description |
|---|---|---|
| `ev.orderId` | `number` | The id the order method returned |
| `ev.symbolId` | `number` | Numeric symbol ID (`ctx.symbol` has the name) |
| `ev.side` | `string` | `"buy"` or `"sell"` |
| `ev.orderType` | `string` | `"MARKET"`, `"LIMIT"`, `"STOP_MARKET"`, ... |
| `ev.status` | `string` | `"ACCEPTED"`, `"PARTIALLY_FILLED"`, `"FILLED"`, `"CANCELED"`, `"REJECTED"`, ... |
| `ev.fillQty`, `ev.fillPrice` | `number` | Quantity and price of this fill; 0 outside a fill |
| `ev.isMaker` | `boolean` | Whether the fill added liquidity |
| `ev.fillRole` | `string \| null` | `"maker"` or `"taker"` on a fill, `null` otherwise |
| `ev.rejectReason` | `string \| null` | Why the order was rejected, `null` otherwise |
| `ev.queueAhead`, `ev.queueTotal` | `number` | Queue position of a resting limit order |
| `ev.marketPosition` | `string \| null` | `"best"`, `"behind_best"`, `"mid_spread"`, `"level_empty"` or `"crossed"` on a market-position event, `null` otherwise |
| `ev.distanceToBestTicks` | `number` | Ticks between the order and the best price on its side |
| `ev.exchangeTsNs` | `bigint` | Timestamp of the event |
| `ev.submittedAtNs`, `ev.acceptedAtNs`, `ev.firstFillAtNs`, `ev.lastFillAtNs`, `ev.canceledAtNs`, `ev.rejectedAtNs`, `ev.triggeredAtNs`, `ev.expiredAtNs` | `bigint` | When the order reached each stage; `0n` for a stage it has not reached |

### `onStart()` / `onStop()`

Called when the strategy starts and stops.

---

## Order methods

All methods accept an options object. `symbol` defaults to the primary symbol when omitted.
Each method that sends an order returns its id, a `number`: the `orderId` its events carry and
the value `cancel` and `modify` take.

### Market

```javascript
this.marketBuy({ qty: 1.0 })
this.marketSell({ symbol: 'ETHUSDT', qty: 2.0 })
```

### Limit

```javascript
this.limitBuy({ price: 50000, qty: 0.1 })
this.limitSell({ price: 51000, qty: 0.1, tif: 'IOC' })  // tif: GTC (default) | IOC | FOK | GTD | POST_ONLY
```

### Stop / take-profit

```javascript
this.stopMarket({ side: 'sell', trigger: 48000, qty: 0.1 })
this.stopLimit({ side: 'buy', trigger: 52000, price: 52100, qty: 0.1 })
this.takeProfitMarket({ side: 'sell', trigger: 55000, qty: 0.1 })
this.takeProfitLimit({ side: 'sell', trigger: 55000, price: 54900, qty: 0.1 })
```

### Trailing stop

```javascript
this.trailingStop({ side: 'sell', offset: 100, qty: 0.1 })
this.trailingStopPercent({ side: 'sell', callbackBps: 50, qty: 0.1 })
```

### Prices and the tick

A QuickJS strategy's symbols are registered without a tick, so its order prices are not checked
against one. Other bindings register the venue's tick and a backtest rejects prices off it with
`rejectReason` `price_off_tick`, `trigger_off_tick` or `trailing_offset_off_tick`; see
[SimulatedExecutor](../api/backtest/simulated_executor.md#order-prices-and-the-tick).

The same holds for quantities: the symbols declare no quantity step, so no order is rejected with
`quantity_off_lot`. A host that registers the symbol itself before loading the script, with a tick
or a lot, has both checked; see
[SimulatedExecutor](../api/backtest/simulated_executor.md#order-quantities-and-the-lot).

### Order management

```javascript
this.cancel(orderId)
this.cancelAll()
this.modify(orderId, { price: 50100, qty: 0.2 })
this.closePosition()
```

Every order method above, from `marketBuy` to `closePosition`, clears the last error on entry.
`lrvx.lastError()`, read right after one inside the callback that called it, is `''` unless that
call left a reason; a reason an earlier call left (a `recompressSegment` that failed, say) is not
read as the order's. A reason ending in `[kill_switch]`, `[order_validator]` or `[risk_manager]` says a
pre-trade gate dropped the signal, on a live runner and in a backtest alike; which signals each of
them puts to the gates is listed under
[Reading why a signal went nowhere](../python/strategy.md#reading-why-a-signal-went-nowhere).

---

## Context queries

```javascript
this.position()             // primary symbol
this.position('ETHUSDT')    // specific symbol
this.bestBid()              // number, or null when there is no bid
this.bestAsk()              // number, or null when there is no ask
this.midPrice()             // number, or null when either side is empty
this.lastPrice()
this.orderStatus(orderId)   // -1 if not found
```

`bestBid`, `bestAsk` and `midPrice` return `null` for an empty side, never a
number. A price below zero is a quote -- WTI settled at -37.63 in April 2020,
day-ahead power clears below zero -- and so is a price of exactly 0, so no
number is free to stand for "no quote". Test with `=== null`, not `=== 0`.
