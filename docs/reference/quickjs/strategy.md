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
| `ctx.position` | `number` | Current position size |
| `ctx.avgEntryPrice` | `number` | Average entry price, `NaN` when the position manager reports none |
| `ctx.book.bidPrice` | `number \| null` | Best bid; `null` when the bid side has no level (`0` is a price) |
| `ctx.book.askPrice` | `number \| null` | Best ask; `null` when the ask side has no level |
| `ctx.book.midPrice` | `number \| null` | Mid price; `null` unless both sides have a level |
| `ctx.book.spread` | `number \| null` | Spread; `null` unless both sides have a level |
| `trade.symbol` | `string` | Symbol name |
| `trade.price` | `number` | Trade price |
| `trade.qty` | `number` | Trade quantity |
| `trade.side` | `string` | `"buy"` or `"sell"` |
| `trade.isBuy` | `boolean` | |
| `trade.timestampNs` | `number` | Nanosecond timestamp |

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
| `bar.startTimeNs`, `bar.endTimeNs` | `number` | Bar window timestamps (nanoseconds) |
| `bar.barType`, `bar.barTypeParam` | `number` | 0=Time, 1=Tick, ... + interval/threshold |
| `bar.closeReason` | `number` | 0=Threshold, 1=Gap, 2=Forced, 3=Warmup. The engine emits `Threshold` for a normal close, `Gap` for the bar that absorbs the remainder of a Renko price jump too wide to walk brick by brick, and `Forced` for a `stop()` flush; `Warmup` is never assigned by the engine itself -- it is for callers who construct their own `Bar` objects from historical data before calling `BarMatrix::warmup()`. |

### `onStart()` / `onStop()`

Called when the strategy starts and stops.

---

## Order methods

All methods accept an options object. `symbol` defaults to the primary symbol when omitted.

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

### Order management

```javascript
this.cancel(orderId)
this.cancelAll()
this.modify(orderId, { price: 50100, qty: 0.2 })
this.closePosition()
```

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
