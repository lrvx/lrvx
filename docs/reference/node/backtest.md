# Backtest components

```javascript
const { BacktestRunner, SimulatedExecutor, BacktestResult, Engine, SignalBuilder } = require('@lrvx/lrvx');
```

---

## BacktestRunner

Replays OHLCV data through a strategy. Emitted orders go to a `SimulatedExecutor`; statistics are returned at the end.

```javascript
const bt = new lrvx.BacktestRunner(registry, feeRate, initialCapital);
bt.setStrategy(strategy);

const stats = bt.runCsv('/path/to/data.csv', 'BTCUSDT');
const stats = bt.runOhlcv(timestampsNs, closePrices, 'BTCUSDT');
const stats = bt.runTapeBars('/path/to/tape', 300, 'BTCUSDT');
```

`new BacktestRunner(registry, feeRate, initialCapital, options?)`: `options` sets the built-in
executor's slippage and latency, the `BacktestConfig` fields camelCased -- `slippageModel`
(`'none'`, `'fixed_ticks'`, `'fixed_bps'`, `'volume_impact'`), `slippageTicks`,
`slippageTickSize`, `slippageBps`, `slippageImpactCoeff`, `submitAckLatencyNs`,
`submitAckJitterNs`, `cancelAckLatencyNs`, `cancelAckJitterNs`, `cancelAckSeed`,
`replaceAckLatencyNs`, `replaceAckJitterNs`, and `latency` (a `ConstantLatency`,
`GaussianLatency`, `ExponentialLatency` or `EmpiricalLatency`). The meaning and the refusals are
the Python ones: see [BacktestRunner constructor](../python/strategy.md#constructor). A runner
built with options refuses `setExecutor`.

| Method | Returns | Description |
|--------|---------|-------------|
| `setStrategy(strategy)` | `void` | Attach a strategy |
| `runCsv(path, symbol)` | stats object | Replay a CSV file (timestamp, open, high, low, close, volume) |
| `runOhlcv(timestamps, closes, symbol)` | stats object | Replay raw arrays (timestamps as Float64Array, closes as Float64Array) |
| `runTape(path)` | stats object | Replay a `.lrvx` tape directory — the canonical recording written by `lrvx tape record` or `scripts/backfill_to_tape.py`. Throws `LrvxError` with the reason on a missing or unreadable tape |
| `runTapeBars(path, intervalSeconds, symbol?)` | stats object | Replay a `.lrvx` tape as time bars: trades aggregated while the tape streams, each closed bar to `onBar` with `runBars`' fill rules. Same result as `readTrades` -> `aggregateTimeBars` -> `runBars` for the same interval. `symbol` is required when the tape holds more than one; throws `LrvxError` on a bad interval, unknown symbol, unreadable tape or a symbol with no trades |
| `runTapeBarsOnTicks(path, intervalSeconds, symbol?)` | stats object | Decide on bars, fill on ticks: `onBar` receives the `runTapeBars` bars, the executor is fed every trade and book record as `runTape` feeds it (`onTrade` and `onBookUpdate` do not fire). An order sent from bar n's `onBar` can first match against the first trade stamped at or after the end of bar n (with a submit latency L, the first record at or after the end + L). `symbol` and the errors as for `runTapeBars`. See [Decide on bars, fill on ticks](../../how-to/backtest.md#decide-on-bars-fill-on-ticks) |
| `runTapes(paths)` | stats object | Merge N `.lrvx` tape directories on read and replay the merged stream. Symbols are rekeyed by `(metadata.exchange, name)` so two venues stay distinct. `runTapes([t])` equals `runTape(t)`; throws `LrvxError` with the reason on bad paths or overlapping book streams |
| `setExecutor(executor)` | `void` | Replace the built-in `SimulatedExecutor`. `null` detaches |
| `addExecutionListener(listener)` | `void` | Attach an order-lifecycle listener. Multiple listeners may be attached; each fires for every order event |
| `equityCurve()` | `EquityCurve` | Equity curve from the most recent run |
| `trades()` | `BacktestTrades` | Closed trades from the most recent run |

`equityCurve()` and `trades()` throw `LrvxError(E_RUN_002)` if no run has completed.

Every run's stats object carries `skippedOptionQuotes` and `skippedPoolStates`: the option-quote
and pool-state records a tape run (`runTape`, `runTapes`, `runTapeBarsOnTicks`) read and delivered
to no callback or the executor, 0 for a run that read no tape.

### Stats object

| Key | Type | Description |
|-----|------|-------------|
| `totalTrades` | `number` | Round-trip trade count |
| `winningTrades` | `number` | Winning trades |
| `losingTrades` | `number` | Losing trades |
| `initialCapital` | `number` | Starting capital |
| `finalCapital` | `number` | Ending capital |
| `netPnl` | `number` | Net P&L after fees |
| `totalPnl` | `number` | Gross P&L |
| `totalFees` | `number` | Total fees paid |
| `grossProfit` | `number` | Sum of winning trades |
| `grossLoss` | `number` | Sum of losing trades |
| `maxDrawdown` | `number` | Max drawdown (absolute) |
| `maxDrawdownPct` | `number` | Max drawdown (%) |
| `winRate` | `number` | Winning trade ratio |
| `profitFactor` | `number` | Gross profit / gross loss |
| `avgWin` | `number` | Average winning trade |
| `avgLoss` | `number` | Average losing trade |
| `sharpeRatio` | `number` | Annualized Sharpe ratio |
| `sortinoRatio` | `number` | Sortino ratio |
| `calmarRatio` | `number` | Calmar ratio |
| `returnPct` | `number` | Net return (%) |

A `BacktestRunner` run's stats object also carries:

| Key | Type | Description |
|-----|------|-------------|
| `avgTradeDurationNs` | `number` | Mean holding time of the closed trades (nanoseconds) |
| `medianTradeDurationNs` | `number` | Median holding time (nanoseconds) |
| `maxTradeDurationNs` | `number` | Longest holding time (nanoseconds) |
| `timeWeightedReturn` | `number` | Time-weighted return |
| `startTimeNs` | `bigint` | Timestamp of the run's first fill; `0n` when nothing filled |
| `endTimeNs` | `bigint` | Timestamp of the run's last fill |

---

## SimulatedExecutor

Lower-level backtest executor. Use directly when you need more control over fill logic than `BacktestRunner` provides.

```javascript
const exec = new lrvx.SimulatedExecutor();
exec.setDefaultSlippage('fixed_bps', 0, 0, 2.0, 0);
exec.setQueueModel('tob', 1);
```

| Method / Property | Description |
|-------------------|-------------|
| `submitOrder(id, side, price, qty, type, symbol, opts?)` | Submit an order (`side`: `"buy"`/`"sell"`) |
| `cancelOrder(orderId)` | Cancel an order |
| `cancelAll(symbol)` | Cancel all orders for a symbol |
| `onBar(symbol, closePrice)` | Feed a bar close |
| `onBarOhlc(symbol, open, high, low, close)` | Feed a full OHLC bar: the manual bar path (see below) |
| `beginBarCallbackWindow()` / `endBarCallbackWindow()` | Hold orders submitted between the two calls until the next bar's open |
| `reset()` | Drop fills and run-scoped state, keeping installed configuration |
| `onTrade(symbol, price, isBuy)` | Feed a trade |
| `onTopOfBook(symbol, bid, ask)` | Feed a touch-only frame. `bid` / `ask` are `{ price, qty }`; `null` means that side of the book is empty |
| `advanceClock(timestampNs)` | Advance simulated time |
| `setDefaultSlippage(model, ticks, tickSize, bps, impactCoeff)` | Configure slippage. `model` is one of `"none"`, `"fixed_ticks"`, `"fixed_bps"`, `"volume_impact"` |
| `setQueueModel(model, depth)` | Configure limit order queue. `model` is one of `"none"`, `"tob"`, `"full"`, `"pro_rata"`, `"pro_rata_with_fifo"`, `"top_pro_lmm"`, `"pro_rata_with_priority"`, or its `QUEUE_*` code; anything else throws with the valid names and leaves the model installed before |
| `fillCount` | Number of fills (property) |

No symbol registry can be attached to this executor, so order prices are not checked against a
tick; `BacktestRunner` checks them.

`submitOrder`'s `type` is one of `"market"`, `"limit"`, `"stop_market"`,
`"stop_limit"`, `"trailing_stop"`. The optional seventh argument carries
`{ tif?: 'gtc' | 'ioc' | 'fok' | 'gtd' | 'post_only', reduceOnly?: boolean, expiresAtNs?: number }`.

### The manual bar path

`onBar` moves the market straight to a bar's close, so a caller driving
`SimulatedExecutor` by hand never sees the bar's open or its intrabar
extremes — a held order would match at a price that only exists because the
bar has already happened. `onBarOhlc` is what `BacktestRunner.runBars` uses
internally: it moves the market to the open first (releasing any order held
from a `beginBarCallbackWindow`/`endBarCallbackWindow` pair at that price),
then walks `low -> high -> close`.

```javascript
const exec = new lrvx.SimulatedExecutor();
exec.advanceClock(60_000_000_000n);
exec.onBarOhlc(1, 50000.0, 50500.0, 49800.0, 50200.0);

exec.beginBarCallbackWindow();
exec.submitOrder(1, 'buy', 0.0, 1.0, 'market', 1);
exec.endBarCallbackWindow();
// The order above is held, not matched, and releases at the next
// onBarOhlc call's open.

exec.reset(); // drop fills before a second hand-driven run
```

---

## BacktestResult

Computes statistics and equity curve from a `SimulatedExecutor`'s fills.

```javascript
const result = new lrvx.BacktestResult(initialCapital, feeRate);
result.ingestExecutor(exec);
const stats = result.stats();
```

| Method | Returns | Description |
|--------|---------|-------------|
| `recordFill(orderId, symbol, side, price, qty, timestampNs)` | `void` | Record a single fill |
| `ingestExecutor(exec)` | `void` | Drain all fills from a `SimulatedExecutor` |
| `stats()` | stats object | Same fields as `BacktestRunner` stats |

---

## Engine

Bulk backtesting engine. Loads OHLCV data once, then runs strategies against it.

The constructor takes capital and fee rate, not a registry.

```javascript
const engine = new lrvx.Engine(100_000, 0.0001);
engine.loadCsv('/path/to/btcusdt_1m.csv');   // symbol inferred from the filename
const signals = new lrvx.SignalBuilder();
signals.buy(tsNs, 1.0);
const stats = engine.run(signals);
```

| Method / Property | Returns | Description |
|-------------------|---------|-------------|
| `new Engine(initialCapital?, feeRate?)` | | Defaults 100000 and 0.0001 (fee is a fraction) |
| `loadCsv(path, symbol?)` | `void` | Load OHLCV CSV. Without `symbol` the name is inferred from the file stem, upper-cased, with a `_1m` / `_5m` / `_15m` / `_1h` / `_4h` / `_1d` suffix stripped |
| `loadOhlcv(bars, symbol?)` | `void` | `bars` is one object `{ts, open, high, low, close, volume}` of `Float64Array`s, not positional arrays. Timestamps in s / ms / us are auto-scaled to ns |
| `resample(src, dst, interval)` | `void` | Three strings, e.g. `('BTCUSDT', 'BTCUSDT_1h', '1h')`. Interval is `<count><s\|m\|h\|d>`. Throws `E_SYM_001` if `src` is not loaded |
| `run(signals)` | `BacktestStats` | Takes a `SignalBuilder` only. Passing a Strategy aborts |
| `barCount(symbol?)` | `number` | Bars loaded; defaults to the first loaded symbol |
| `ts(symbol?)` | `BigInt64Array` | Bar open times in nanoseconds. Exact int64 — a double steps 256 ns at present-day magnitudes |
| `open(symbol?)` | `Float64Array` | Open prices |
| `high(symbol?)` | `Float64Array` | High prices |
| `low(symbol?)` | `Float64Array` | Low prices |
| `close(symbol?)` | `Float64Array` | Close prices |
| `volume(symbol?)` | `Float64Array` | Volumes |
| `symbols` | `string[]` | Registered symbol names (property) |

`run()` returns the full `BacktestStats` shape, not the narrower runner
shape — see [Stats object](#stats-object). The key names are the same in
both; only the field set differs.

`Engine` symbols are names with no tick, so limit prices are not checked
against one; a `BacktestRunner` on a registry with a tick rejects an
off-tick price ([SimulatedExecutor](../api/backtest/simulated_executor.md#order-prices-and-the-tick)).

---

## SignalBuilder

Builds a signal array to pass to `engine.run()`.

Signals are keyed by timestamp and carry a quantity — not a bar index.

```javascript
const signals = new lrvx.SignalBuilder();
signals.buy(tsNs, 1.0)              // methods return `this`, so they chain
       .limitSell(tsNs, 101.0, 1.0);
const stats = engine.run(signals);
```

| Method / Property | Description |
|-------------------|-------------|
| `buy(tsNs, qty, symbol?)` | Long entry at `tsNs`. Returns `this` |
| `sell(tsNs, qty, symbol?)` | Short entry. Returns `this` |
| `limitBuy(tsNs, price, qty, symbol?)` | Limit long entry. Returns `this` |
| `limitSell(tsNs, price, qty, symbol?)` | Limit short entry. Returns `this` |
| `clear()` | Clear all signals |
| `length` | Signal count (read-only property) |

`tsNs` takes a JS `number` or a `bigint`, so an `Engine.ts()` element goes
straight in; values in s / ms / us are auto-scaled to ns.
| `length` | Number of signals (property) |
