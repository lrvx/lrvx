# Node.js Bindings

Event-driven live trading and backtesting from Node.js using `lrvx`.

## Install / Build

```bash
npm install @lrvx/lrvx
# or build from source:
cd node && npm install && npm run build
```

The addon is built out of tree by npm, not by CMake. `LRVX_BUILD_NODE` exists for parity with the other binding flags but does not invoke npm. The compiled addon lands at `node/build/Release/lrvx_node.node`.

```javascript
const lrvx = require('@lrvx/lrvx');
// or, if building from source:
const lrvx = require('../node');
```

## SymbolRegistry and Symbol

```javascript
const registry = new lrvx.SymbolRegistry();
const btc = registry.addSymbol('binance', 'BTCUSDT', 0.01);

btc.id        // 1
btc.name      // "BTCUSDT"
btc.exchange  // "binance"
btc.tickSize  // 0.01

Number(btc)   // 1
btc.valueOf() // 1
btc + 0       // 1
btc.toString() // "Symbol(binance:BTCUSDT, id=1)"
// btc works as a number wherever a symbol ID is expected
```

## Strategy

A strategy is a plain JavaScript object with callback properties.

```javascript
const strategy = {
    symbols: [btc],

    onStart() {},
    onStop() {},

    onTrade(ctx, trade, emit) {
        // ctx.position      — current position quantity
        // ctx.symbolId      — symbol ID (int)
        // ctx.lastTradePrice
        // ctx.bestBid      — null when the bid side has no level (0 is a price)
        // ctx.bestAsk      — null when the ask side has no level
        // ctx.midPrice     — null unless both sides have a level

        // trade.price
        // trade.qty
        // trade.isBuy       — boolean
        // trade.side        — "buy" | "sell"
        // trade.timestampNs — BigInt nanoseconds

        if (ctx.position === 0) {
            emit.marketBuy(0.01);
        }
    },

    onBookUpdate(ctx, emit) {},
};
```

### emit methods

| Method | Description |
|--------|-------------|
| `emit.marketBuy(qty)` | Market buy |
| `emit.marketSell(qty)` | Market sell |
| `emit.limitBuy(price, qty)` | Limit buy |
| `emit.limitSell(price, qty)` | Limit sell |
| `emit.provideLiquidity(priceLower, priceUpper, liquidity)` | Add AMM liquidity in a tick range |
| `emit.withdrawLiquidity(liquidity)` | Remove AMM liquidity |
| `emit.cancel(orderId)` | Cancel order |
| `emit.closePosition()` | Close position (reduce-only) |

## Runner

```javascript
function onSignal(sig) {
    // sig.side       — "buy" | "sell"
    // sig.quantity
    // sig.price      — 0 for market orders
    // sig.orderType  — "market" | "limit" | ...
    // sig.orderId
}

const runner = new lrvx.Runner(registry, onSignal);        // synchronous
const runner = new lrvx.Runner(registry, onSignal, true);  // Disruptor background thread

runner.addStrategy(strategy);
runner.start();

// Inject market data from your feed:
runner.onTrade(btc, price, qty, isBuy, tsNs);
runner.onBookSnapshot(btc, bidPrices, bidQtys, askPrices, askQtys, tsNs);

runner.stop();
```

`btc` in feed methods accepts a `Symbol` object or a raw number.

## Backtest

```javascript
const bt = new lrvx.BacktestRunner(registry, 0.0004, 10_000);
bt.setStrategy(strategy);

const stats = bt.runCsv('/path/to/btcusdt_1m.csv', 'BTCUSDT');
```

The constructor takes `(registry, feeRate, initialCapital)` — nothing else. Flat fee rate, no funding, no liquidation, no rate limits, no queue position. Good for an indicator sanity check; not enough before live.

`runCsv` reads OHLCV **bars**: one header line, then `timestamp,open,high,low,close,volume`. Only the timestamp and close columns are used, and each bar is replayed as one trade. Other entry points: `runOhlcv(timestamps, closes, symbol)`, `runTape(path)`, `runTapes(paths)`, `runTapeBars(path, intervalSeconds, symbol)`, which replays a tape as time bars, and `runTapeBarsOnTicks(path, intervalSeconds, symbol)`, which shows the strategy the same bars while the executor fills on the tape's prints.

Hooks attach after construction: `setExecutor(executor)` (an object implementing the `Executor` interface, or `null`), `addExecutionListener(listener)`. Run artifacts: `equityCurve()`, `trades()`.

### Stats object

| Key | Description |
|-----|-------------|
| `returnPct` | Net return percentage |
| `netPnl` | Net P&L after fees |
| `totalTrades` | Round-trip trade count |
| `winRate` | Winning trade fraction |
| `sharpeRatio` | Annualized Sharpe ratio |
| `maxDrawdownPct` | Peak-to-trough drawdown (%) |

Also present: `winningTrades`, `losingTrades`, `initialCapital`, `finalCapital`, `totalFees`, `maxDrawdown`, `profitFactor`, `avgWin`, `avgLoss`.

Every producer uses the same names: `sharpeRatio`, `sortinoRatio` and `calmarRatio`. What differs is the field set, and `index.d.ts` has one interface per shape — `BacktestStats` for `BacktestResult.stats()`, `RunnerStats` for `BacktestRunner.runCsv` / `runOhlcv` / `runBars`, and `FoldStats` for `GridSearch` and `WalkForwardRunner`.

### Venue physics

`VenueStack` is a separate venue simulation with its own flat proxy surface. It is not an argument to `BacktestRunner` and does not attach to one.

```javascript
const stack = lrvx.VenueStack.binanceUmFutures(42, 10_000);

stack.accountOpenPosition(btc, 5.0, 50_000);
stack.accountSetMark(btc, 47_000);
const liquidated = stack.liquidationOnMark(btc, 47_000);
stack.feesRecordFill(tsNs, 20_000);
```

One call wires the cross-margin account, MM tiers and ADL, the VIP fee schedule, funding settlement, rate limits, and the venue-availability hook. Other factories: `bybitLinear`, `okxSwap`, `deribit`, plus `VenueStack.fromVenue(name, accountId, equity)`.

Full pattern and pieces: [Realistic backtest in one call](../how-to/realistic-backtest.md), [Cross-margin accounts](../how-to/cross-margin.md), [Liquidation and ADL](../how-to/liquidation-and-adl.md).

## Full Example — SMA Crossover

```javascript
const lrvx = require('@lrvx/lrvx');

const registry = new lrvx.SymbolRegistry();
const btc = registry.addSymbol('binance', 'BTCUSDT', 0.01);

// Simple SMA helper
function makeSMA(period) {
    const buf = [];
    return {
        update(price) {
            buf.push(price);
            if (buf.length > period) buf.shift();
            if (buf.length < period) return null;
            return buf.reduce((a, b) => a + b, 0) / period;
        },
    };
}

const fast = makeSMA(10);
const slow = makeSMA(30);

const strategy = {
    symbols: [btc],

    onTrade(ctx, trade, emit) {
        const f = fast.update(trade.price);
        const s = slow.update(trade.price);
        if (f === null || s === null) return;

        if (f > s && ctx.position === 0) {
            emit.marketBuy(0.01);
        } else if (f < s && ctx.position > 0) {
            emit.closePosition();
        }
    },
};

// --- Live ---
function onSignal(sig) {
    console.log(sig.side, sig.orderType, sig.quantity);
    // forward to exchange
}

const runner = new lrvx.Runner(registry, onSignal);
runner.addStrategy(strategy);
runner.start();
// runner.onTrade(btc, price, qty, isBuy, tsNs)  ← from your market data feed
// runner.stop()

// --- Backtest ---
const bt = new lrvx.BacktestRunner(registry, 0.0004, 10_000);
bt.setStrategy(strategy);
const stats = bt.runCsv('./data/btcusdt_1m.csv', 'BTCUSDT');
console.log(stats);
```
