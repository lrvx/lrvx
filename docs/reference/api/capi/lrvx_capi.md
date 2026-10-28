# C API reference

```c
#include "lrvx/capi/lrvx_capi.h"
```

All language bindings (Python, Node.js, Codon, embedded JS) call into this header. You can use it directly to integrate lrvx into any language with a C FFI, or embed it in a C/C++ project.

!!! warning "This page is a curated subset"
    The full surface is **729 functions, 58 handles, 59 structs, 43 callback typedefs, 3 enums**
    across 72 groups. This page covers the commonly used core (roughly 150 functions) with prose.
    The complete, machine-generated reference — every function signature, in group order — is
    [`tools/codegen/golden/lrvx_capi.md`](https://github.com/lrvx/lrvx/blob/main/tools/codegen/golden/lrvx_capi.md),
    generated from `include/lrvx/capi/lrvx_capi_spec.hpp`. **That file is the reference of record.**
    When this page and the header disagree, the header wins.

    Groups with no prose coverage here: Account, Amm Curve, Delta Book, Dex Amount, Execution Algos,
    Fee Schedule, Feed Clock, Lrvxrun/trace, Funding Schedule, Grid Search, Heatmap, Indicator Graph,
    Latency Models, Liquidation Engine, Live Queue Position, Logger, Merged Tape Reader, Metrics,
    Multi-TF Helpers, Order Group, Order Journey Tracer, Pool Tape, Portfolio Risk, Rate Limit,
    Recorder, Replay, Risk, Storage, Tape Aggregator, Tape Diff, Targets, Trace Attach, Validation,
    Venue Availability, Venue Stack, Walk Forward.

---

## Opaque handles

All 58 handles are `void*`. Lifetime is managed by the matching `_create` / `_destroy` pair.

```c
typedef void* LrvxStrategyHandle;
typedef void* LrvxRegistryHandle;
typedef void* LrvxBookHandle;
typedef void* LrvxSimulatedExecutorHandle;
typedef void* LrvxPositionTrackerHandle;
typedef void* LrvxPositionGroupHandle;
typedef void* LrvxOrderTrackerHandle;
typedef void* LrvxOrderJourneyTracerHandle;
typedef void* LrvxFootprintHandle;
typedef void* LrvxVolumeProfileHandle;
typedef void* LrvxMarketProfileHandle;
typedef void* LrvxCompositeBookHandle;
typedef void* LrvxCurveHandle;
typedef void* LrvxPoolTapeHandle;
typedef void* LrvxPoolReplayHandle;
typedef void* LrvxIndicatorGraphHandle;
typedef LrvxIndicatorGraphHandle LrvxStreamingGraphHandle;  // alias
typedef void* LrvxOrderGroupHandle;
typedef void* LrvxFeedClockHandle;
typedef void* LrvxL3BookHandle;
typedef void* LrvxDataWriterHandle;
typedef void* LrvxDataReaderHandle;
typedef void* LrvxLatencyDistributionHandle;
typedef void* LrvxRateLimitPolicyHandle;
typedef void* LrvxVenueAvailabilityHandle;
typedef void* LrvxBacktestResultHandle;
typedef void* LrvxMergedTapeReaderHandle;
typedef void* LrvxPartitionerHandle;
typedef void* LrvxRiskManagerHandle;
typedef void* LrvxKillSwitchHandle;
typedef void* LrvxOrderValidatorHandle;
typedef void* LrvxPnLTrackerHandle;
typedef void* LrvxStorageSinkHandle;
typedef void* LrvxMarketDataRecorderHandle;
typedef void* LrvxBinaryLogRecorderHookHandle;
typedef void* LrvxReplaySourceHandle;
typedef void* LrvxExecutionListenerHandle;
typedef void* LrvxExecutorHandle;
typedef void* LrvxLiveEngineHandle;
typedef void* LrvxRunnerHandle;
typedef void* LrvxBacktestRunnerHandle;
typedef void* LrvxGridSearchHandle;
typedef void* LrvxLatencyModelHandle;
typedef void* LrvxTapeDiffHandle;
typedef void* LrvxPortfolioRiskHandle;
typedef void* LrvxExecAlgoHandle;
typedef void* LrvxDeltaBookEncoderHandle;
typedef void* LrvxDeltaBookReplayerHandle;
typedef void* LrvxRunRecorderHandle;
typedef void* LrvxRunReaderHandle;
typedef void* LrvxBarDispatchRecorderHandle;
typedef void* LrvxAggregatorHandle;
typedef void* LrvxFeeScheduleHandle;
typedef void* LrvxFundingScheduleHandle;
typedef void* LrvxLiveQueuePositionHandle;
typedef void* LrvxLiquidationEngineHandle;
typedef void* LrvxAccountHandle;
typedef void* LrvxVenueStackHandle;
```

---

## Structs

### `LrvxTradeData`

| Field | Type | Description |
|-------|------|-------------|
| `symbol` | `uint32_t` | Symbol ID |
| `price_raw` | `int64_t` | Price × 1e8 |
| `quantity_raw` | `int64_t` | Quantity × 1e8 |
| `is_buy` | `uint8_t` | 1 = buy, 0 = sell |
| `exchange_ts_ns` | `int64_t` | Exchange timestamp (ns) |

### `LrvxBookLevel`

| Field | Type | Description |
|-------|------|-------------|
| `price_raw` | `int64_t` | Price × 1e8 |
| `quantity_raw` | `int64_t` | Quantity × 1e8 |

### `LrvxBookSnapshot`

| Field | Type | Description |
|-------|------|-------------|
| `bid_price_raw` | `int64_t` | Best bid price × 1e8; a price only when `has_bid` is 1, else 0 |
| `bid_qty_raw` | `int64_t` | Size resting at the best bid × 1e8 (0 when the book cannot say) |
| `ask_price_raw` | `int64_t` | Best ask price × 1e8; a price only when `has_ask` is 1, else 0 |
| `ask_qty_raw` | `int64_t` | Size resting at the best ask × 1e8 (0 when the book cannot say) |
| `mid_raw` | `int64_t` | Mid price × 1e8; a price only when both flags are 1, else 0 |
| `spread_raw` | `int64_t` | Spread × 1e8; a price only when both flags are 1, else 0 |
| `has_bid` | `uint8_t` | 1 when the bid side has a best level, at any price, 0 included |
| `has_ask` | `uint8_t` | 1 when the ask side has a best level, at any price, 0 included |

A price field of 0 is not "no quote": a book can be quoted at or below zero
(see `lrvx_best_bid_raw_opt`). Read the flag before the price. The flags
arrived with ABI version 4.

### `LrvxBookData`

| Field | Type | Description |
|-------|------|-------------|
| `symbol` | `uint32_t` | Symbol ID |
| `exchange_ts_ns` | `int64_t` | Exchange timestamp (ns) |
| `snapshot` | `LrvxBookSnapshot` | Top-of-book snapshot |

### `LrvxSymbolContext`

| Field | Type | Description |
|-------|------|-------------|
| `symbol_id` | `uint32_t` | Symbol ID |
| `position_raw` | `int64_t` | Position × 1e8 |
| `avg_entry_price_raw` | `int64_t` | Average entry price × 1e8 |
| `last_trade_price_raw` | `int64_t` | Last trade price × 1e8 |
| `last_update_ns` | `int64_t` | Last update timestamp (ns) |
| `book` | `LrvxBookSnapshot` | Top-of-book snapshot |
| `last_touch_ns` | `int64_t` | Timestamp (ns) of the frame that last wrote the touch; can be fresher than `last_update_ns`, which stamps dispatched events only |

### `LrvxBarData`

Passed to `LrvxOnBarCallback` when a bar closes.

| Field | Type | Description |
|-------|------|-------------|
| `symbol` | `uint32_t` | Symbol ID |
| `bar_type` | `uint8_t` | Bar type (Time, Tick, Volume, Range, Renko, Dollar, BpsRange) |
| `close_reason` | `uint8_t` | Why the bar closed |
| `_pad` | `uint8_t[2]` | Padding |
| `bar_type_param` | `uint64_t` | Type parameter. For Time bars this is **nanoseconds** (1-minute = `60000000000`) |
| `open_raw` | `int64_t` | Open × 1e8 |
| `high_raw` | `int64_t` | High × 1e8 |
| `low_raw` | `int64_t` | Low × 1e8 |
| `close_raw` | `int64_t` | Close × 1e8 |
| `volume_raw` | `int64_t` | Volume × 1e8 |
| `buy_volume_raw` | `int64_t` | Buy-side volume × 1e8 |
| `trade_count_raw` | `int64_t` | Trade count × 1e8 |
| `start_time_ns` | `int64_t` | Bar open time (ns) |
| `end_time_ns` | `int64_t` | Bar close time (ns) |

`LrvxBarData` is distinct from `LrvxBar` (below). `LrvxBar` is the batch-aggregation output row;
`LrvxBarData` is the live strategy callback payload.

### `LrvxStrategyCallbacks`

**10 members.** Field order is load-bearing for FFI consumers — allocate the full struct
(10 pointer-sized slots on a 64-bit target) and zero it before filling. A short allocation
corrupts memory when the runtime writes past the end.

| Field | Type | Description |
|-------|------|-------------|
| `on_trade` | `LrvxOnTradeCallback` | Trade event callback |
| `on_book` | `LrvxOnBookCallback` | Book update callback |
| `on_bar` | `LrvxOnBarCallback` | Closed OHLC bar callback (`LrvxBarData`) |
| `on_start` | `LrvxOnStartCallback` | Strategy start callback |
| `on_stop` | `LrvxOnStopCallback` | Strategy stop callback |
| `on_fill` | `LrvxOnFillCallback` | Fill callback (`LrvxOrderEventData`) |
| `on_order_update` | `LrvxOnOrderUpdateCallback` | Order status change (`LrvxOrderEventData`) |
| `on_queue_position_change` | `LrvxOnQueuePositionChangeCallback` | Queue position moved |
| `on_market_position_change` | `LrvxOnMarketPositionChangeCallback` | Order's position vs best moved |
| `user_data` | `void*` | Passed to all callbacks |

Any member may be `NULL`; an unset callback is a no-op.

### `LrvxOrderEventData`

Payload for `on_fill`, `on_order_update`, `on_queue_position_change`, `on_market_position_change`.

| Field | Type | Description |
|-------|------|-------------|
| `order_id` | `uint64_t` | Order ID |
| `symbol_id` | `uint32_t` | Symbol ID |
| `side` | `uint8_t` | 0 = buy, 1 = sell |
| `order_type` | `uint8_t` | `OrderType` encoding — see [order_type encodings](#order_type-encodings) |
| `status` | `uint8_t` | `OrderEventStatus` value |
| `_pad` | `uint8_t` | Padding |
| `fill_qty_raw` | `int64_t` | Fill quantity × 1e8 |
| `fill_price_raw` | `int64_t` | Fill price × 1e8 |
| `exchange_ts_ns` | `int64_t` | Exchange timestamp (ns) |
| `reject_reason` | `const char*` | Reject reason, or `NULL`. Borrowed; valid only for the callback's duration |
| `queue_ahead_raw` | `int64_t` | Queue volume ahead × 1e8 |
| `queue_total_raw` | `int64_t` | Total queue volume at the level × 1e8 |
| `submitted_at_ns` | `int64_t` | Submit timestamp (ns) |
| `accepted_at_ns` | `int64_t` | Accept timestamp (ns) |
| `first_fill_at_ns` | `int64_t` | First fill timestamp (ns) |
| `last_fill_at_ns` | `int64_t` | Last fill timestamp (ns) |
| `canceled_at_ns` | `int64_t` | Cancel timestamp (ns) |
| `rejected_at_ns` | `int64_t` | Reject timestamp (ns) |
| `triggered_at_ns` | `int64_t` | Trigger timestamp (ns) |
| `expired_at_ns` | `int64_t` | Expiry timestamp (ns) |
| `is_maker` | `uint8_t` | 1 if the fill was passive |
| `market_position` | `uint8_t` | `MarketPosition` value |
| `distance_to_best_ticks` | `int32_t` | Ticks from best price |
| `_pad2` | `uint8_t[2]` | Padding |

### `LrvxSignal`

Emitted by strategies, received by the order backend.

| Field | Type | Description |
|-------|------|-------------|
| `order_id` | `uint64_t` | Order ID |
| `symbol` | `uint32_t` | Symbol ID |
| `side` | `uint8_t` | 0 = buy, 1 = sell |
| `order_type` | `uint8_t` | **Encoding depends on the entry point** — see [order_type encodings](#order_type-encodings) |
| `price` | `double` | Limit price (0 for market orders) |
| `quantity` | `double` | Order quantity |
| `trigger_price` | `double` | Stop/take-profit trigger |
| `trailing_offset` | `double` | Trailing stop absolute offset |
| `trailing_bps` | `int32_t` | Trailing stop callback rate (basis points) |
| `new_price` | `double` | Modify: updated price |
| `new_quantity` | `double` | Modify: updated quantity |
| `range_lower` | `double` | Provide/withdraw liquidity: lower bound of the range |
| `range_upper` | `double` | Provide liquidity: upper bound of the range (0 on withdraw) |
| `liquidity` | `double` | Provide/withdraw liquidity: position size |

### `LrvxBar`

| Field | Type | Description |
|-------|------|-------------|
| `start_time_ns` | `int64_t` | Bar open time (ns) |
| `end_time_ns` | `int64_t` | Bar close time (ns) |
| `open_raw` | `int64_t` | Open × 1e8 |
| `high_raw` | `int64_t` | High × 1e8 |
| `low_raw` | `int64_t` | Low × 1e8 |
| `close_raw` | `int64_t` | Close × 1e8 |
| `volume_raw` | `int64_t` | Volume × 1e8 |
| `buy_volume_raw` | `int64_t` | Buy-side volume × 1e8 |
| `trade_count` | `uint32_t` | Number of trades |

### `LrvxFill`

| Field | Type | Description |
|-------|------|-------------|
| `order_id` | `uint64_t` | Order ID |
| `symbol` | `uint32_t` | Symbol ID |
| `side` | `uint8_t` | 0 = buy, 1 = sell |
| `price_raw` | `int64_t` | Fill price × 1e8 |
| `quantity_raw` | `int64_t` | Fill quantity × 1e8 |
| `timestamp_ns` | `int64_t` | Fill timestamp (ns) |

### `LrvxBacktestStats`

| Field | Type | Description |
|-------|------|-------------|
| `totalTrades` | `uint64_t` | Round-trip trade count |
| `winningTrades` | `uint64_t` | Winning trades |
| `losingTrades` | `uint64_t` | Losing trades |
| `maxConsecutiveWins` | `uint64_t` | Max consecutive wins |
| `maxConsecutiveLosses` | `uint64_t` | Max consecutive losses |
| `initialCapital` | `double` | Starting capital |
| `finalCapital` | `double` | Ending capital |
| `totalPnl` | `double` | Gross PnL |
| `totalFees` | `double` | Total fees paid |
| `netPnl` | `double` | Net PnL after fees |
| `grossProfit` | `double` | Sum of winning trades |
| `grossLoss` | `double` | Sum of losing trades |
| `maxDrawdown` | `double` | Max drawdown (absolute) |
| `maxDrawdownPct` | `double` | Max drawdown (%) |
| `winRate` | `double` | Winning trade ratio |
| `profitFactor` | `double` | Gross profit / gross loss |
| `avgWin` | `double` | Average winning trade |
| `avgLoss` | `double` | Average losing trade |
| `avgWinLossRatio` | `double` | avgWin / avgLoss |
| `avgTradeDurationNs` | `double` | Average trade duration (ns) |
| `medianTradeDurationNs` | `double` | Median trade duration (ns) |
| `maxTradeDurationNs` | `double` | Longest trade (ns) |
| `sharpeRatio` | `double` | Annualized Sharpe ratio |
| `sortinoRatio` | `double` | Sortino ratio |
| `calmarRatio` | `double` | Calmar ratio |
| `timeWeightedReturn` | `double` | Time-weighted return |
| `returnPct` | `double` | Net return (%) |
| `startTimeNs` | `int64_t` | Backtest start timestamp (ns) |
| `endTimeNs` | `int64_t` | Backtest end timestamp (ns) |

### `LrvxEquityPoint`

| Field | Type | Description |
|-------|------|-------------|
| `timestamp_ns` | `int64_t` | Timestamp (ns) |
| `equity` | `double` | Equity at this point |
| `drawdown_pct` | `double` | Drawdown (%) at this point |

---

## order_type encodings

There are **two different `uint8_t` order-type encodings** in this API. They disagree on the two
most common values. Which one applies depends on the entry point, not on the struct.

**Encoding A — SignalType (0 = market, 1 = limit)**

| Value | Meaning |
|-------|---------|
| 0 | market |
| 1 | limit |
| 2 | stop_market |
| 3 | stop_limit |
| 4 | tp_market |
| 5 | tp_limit |
| 6 | trailing_stop |
| 7 | cancel |
| 8 | cancel_all |
| 9 | modify |

**Encoding B — C++ `lrvx::OrderType` (0 = LIMIT, 1 = MARKET)**

| Value | Meaning |
|-------|---------|
| 0 | LIMIT |
| 1 | MARKET |
| 2 | STOP_MARKET |
| 3 | STOP_LIMIT |
| 4 | TAKE_PROFIT_MARKET |
| 5 | TAKE_PROFIT_LIMIT |
| 6 | TRAILING_STOP |
| 7 | ICEBERG |

Values 2–6 coincide; 0, 1, 7 and above do not. Which entry point uses which:

| Entry point | Field | Encoding | Code path |
|-------------|-------|----------|-----------|
| `LrvxOnSignalCallback` from the runner / live engine | `LrvxSignal.order_type` | **A** (SignalType) | `src/capi/lrvx_capi.cpp` — explicit `switch (sig.type)` |
| `LrvxRiskManagerAllowFn` | `LrvxSignal.order_type` | **A** (SignalType) | `orderToLrvxSignal()` — `signalTypeCodeFromOrderType(order.type)` |
| `LrvxKillSwitchCheckFn` | `LrvxSignal.order_type` | **A** (SignalType) | `orderToLrvxSignal()` — `signalTypeCodeFromOrderType(order.type)` |
| `LrvxOrderValidatorValidateFn` | `LrvxSignal.order_type` | **A** (SignalType) | `orderToLrvxSignal()` — `signalTypeCodeFromOrderType(order.type)` |
| `LrvxPnLTrackerOnSignalFn` | `LrvxSignal.order_type` | **A** (SignalType) | `orderToLrvxSignal()` — `signalTypeCodeFromOrderType(order.type)` |
| `lrvx_simulated_executor_submit_order` | `order_type` argument | **B** (`OrderType`) | `static_cast<OrderType>(order_type)` |
| `lrvx_simulated_executor_submit_order_ex` | `order_type` argument | **B** (`OrderType`) | `static_cast<OrderType>(order_type)` |
| `LrvxOrderEventData.order_type` | struct field | **B** (`OrderType`) | carried from `Order::type` |

The four pre-trade gate rows (risk manager, kill switch, order validator, PnL
tracker) used to hand the gate a raw `static_cast<uint8_t>(order.type)`,
which is Encoding B, even though the field is `LrvxSignal.order_type` and
every other producer on this page fills it with Encoding A.
`orderToLrvxSignal()` now converts explicitly through
`signalTypeCodeFromOrderType()` (see `src/capi/lrvx_capi.cpp`), so all four
gates receive the same encoding as `LrvxOnSignalCallback`. A gate written
against the old table would have read every LIMIT order it received as
MARKET, and vice versa.

Consequence: passing `0` to `lrvx_simulated_executor_submit_order` submits a **LIMIT** order, not a
market order. Pass `1` for market. The embedded QuickJS binding compensates for this mismatch by
remapping in `src/quickjs/js_strategy.cpp` before it calls through; bindings you write yourself must
do the same.

---

## Callback types

The header declares 43 callback typedefs. The strategy, gate and progress callbacks are below; the
listener, executor, recorder and replay-source function pointers are in the generated reference.

Strategy callbacks — the members of `LrvxStrategyCallbacks`:

```c
typedef void (*LrvxOnTradeCallback)(void* user_data, const LrvxSymbolContext* ctx,
                                    const LrvxTradeData* trade);
typedef void (*LrvxOnBookCallback)(void* user_data, const LrvxSymbolContext* ctx,
                                   const LrvxBookData* book);
typedef void (*LrvxOnBarCallback)(void* user_data, const LrvxSymbolContext* ctx,
                                  const LrvxBarData* bar);
typedef void (*LrvxOnStartCallback)(void* user_data);
typedef void (*LrvxOnStopCallback)(void* user_data);
typedef void (*LrvxOnFillCallback)(void* user_data, const LrvxSymbolContext* ctx,
                                   const LrvxOrderEventData* ev);
typedef void (*LrvxOnOrderUpdateCallback)(void* user_data, const LrvxSymbolContext* ctx,
                                          const LrvxOrderEventData* ev);
typedef void (*LrvxOnQueuePositionChangeCallback)(void* user_data, const LrvxSymbolContext* ctx,
                                                  const LrvxOrderEventData* ev);
typedef void (*LrvxOnMarketPositionChangeCallback)(void* user_data, const LrvxSymbolContext* ctx,
                                                   const LrvxOrderEventData* ev);
```

Signal sink and pre-trade gates. The gates run in order KillSwitch, OrderValidator, RiskManager;
returning 0 drops the signal and skips the remaining gates. Note the `LrvxSignal.order_type` in a
gate callback uses [encoding B](#order_type-encodings), not the runner's encoding A:

```c
typedef void    (*LrvxOnSignalCallback)(void* user_data, const LrvxSignal* signal);
typedef uint8_t (*LrvxRiskManagerAllowFn)(void* user_data, const LrvxSignal* signal);
typedef uint8_t (*LrvxKillSwitchCheckFn)(void* user_data, const LrvxSignal* signal);
typedef uint8_t (*LrvxOrderValidatorValidateFn)(void* user_data, const LrvxSignal* signal);
typedef void    (*LrvxPnLTrackerOnSignalFn)(void* user_data, const LrvxSignal* signal);
typedef void    (*LrvxStorageSinkStoreFn)(void* user_data, const LrvxSignal* signal);
```

Logging and long-running-operation progress. `LrvxProgressCallback` returns 0 to request cancellation:

```c
typedef void    (*LrvxLogCallback)(void* user_data, int32_t level, const char* message);
typedef uint8_t (*LrvxProgressCallback)(void* user_data, double fraction, int64_t items_done);
```

---

## Symbol registry

```c
LrvxRegistryHandle lrvx_registry_create(void);
void               lrvx_registry_destroy(LrvxRegistryHandle registry);

uint32_t lrvx_registry_add_symbol(LrvxRegistryHandle registry,
                                  const char* exchange, const char* name,
                                  double tick_size);

uint8_t  lrvx_registry_get_symbol_id(LrvxRegistryHandle registry,
                                     const char* exchange, const char* name,
                                     uint32_t* id_out);
uint8_t  lrvx_registry_get_symbol_name(LrvxRegistryHandle registry,
                                       uint32_t symbol_id,
                                       char* exchange_out, size_t exchange_len,
                                       char* name_out, size_t name_len);
uint32_t lrvx_registry_symbol_count(LrvxRegistryHandle registry);
```

---

## Strategy

```c
LrvxStrategyHandle lrvx_strategy_create(uint32_t id,
                                        const uint32_t* symbols, uint32_t num_symbols,
                                        LrvxRegistryHandle registry,
                                        LrvxStrategyCallbacks callbacks);
void lrvx_strategy_destroy(LrvxStrategyHandle strategy);
```

---

## StrategyRunner

Synchronous strategy host. Strategy callbacks fire in the caller's thread before the push call returns.

```c
LrvxRunnerHandle lrvx_runner_create(LrvxRegistryHandle registry,
                                    LrvxOnSignalCallback on_signal,
                                    void* user_data);
void lrvx_runner_destroy(LrvxRunnerHandle runner);

void lrvx_runner_add_strategy(LrvxRunnerHandle runner, LrvxStrategyHandle strategy);
void lrvx_runner_start(LrvxRunnerHandle runner);
void lrvx_runner_stop(LrvxRunnerHandle runner);

void lrvx_runner_on_trade(LrvxRunnerHandle runner, uint32_t symbol,
                          double price, double qty, uint8_t is_buy,
                          int64_t exchange_ts_ns);
void lrvx_runner_on_book_snapshot(LrvxRunnerHandle runner, uint32_t symbol,
                                  const double* bid_prices, const double* bid_qtys,
                                  uint32_t n_bids,
                                  const double* ask_prices, const double* ask_qtys,
                                  uint32_t n_asks, int64_t exchange_ts_ns);
```

---

## LiveEngine

Disruptor-based live trading engine. Each strategy runs in its own consumer thread. Publish calls are lock-free and return immediately.

```c
LrvxLiveEngineHandle lrvx_live_engine_create(LrvxRegistryHandle registry);
void                 lrvx_live_engine_destroy(LrvxLiveEngineHandle engine);

void lrvx_live_engine_add_strategy(LrvxLiveEngineHandle engine,
                                   LrvxStrategyHandle strategy,
                                   LrvxOnSignalCallback on_signal,
                                   void* user_data);

void lrvx_live_engine_start(LrvxLiveEngineHandle engine);
void lrvx_live_engine_stop(LrvxLiveEngineHandle engine);

void lrvx_live_engine_publish_trade(LrvxLiveEngineHandle engine,
                                    uint32_t symbol,
                                    double price, double qty, uint8_t is_buy,
                                    int64_t exchange_ts_ns);
void lrvx_live_engine_publish_book_snapshot(LrvxLiveEngineHandle engine,
                                            uint32_t symbol,
                                            const double* bid_prices,
                                            const double* bid_qtys, uint32_t n_bids,
                                            const double* ask_prices,
                                            const double* ask_qtys, uint32_t n_asks,
                                            int64_t exchange_ts_ns);
```

---

## BacktestRunner

Replays OHLCV data through a strategy and returns statistics.

```c
LrvxBacktestRunnerHandle lrvx_backtest_runner_create(LrvxRegistryHandle registry,
                                                     double fee_rate,
                                                     double initial_capital);
void lrvx_backtest_runner_destroy(LrvxBacktestRunnerHandle runner);

void lrvx_backtest_runner_set_strategy(LrvxBacktestRunnerHandle runner,
                                       LrvxStrategyHandle strategy);

// Replay a CSV file (columns: timestamp, open, high, low, close, volume).
// Returns 1 on success, 0 on error.
int lrvx_backtest_runner_run_csv(LrvxBacktestRunnerHandle runner,
                                 const char* path, const char* symbol,
                                 LrvxBacktestStats* stats_out);

// Replay raw OHLCV arrays (timestamps in nanoseconds).
// Returns 1 on success, 0 on error.
int lrvx_backtest_runner_run_ohlcv(LrvxBacktestRunnerHandle runner,
                                   const int64_t* timestamps_ns,
                                   const double* close_prices, uint32_t n,
                                   const char* symbol,
                                   LrvxBacktestStats* stats_out);
```

---

## Signal emission

All return `OrderId` (`uint64_t`), 0 on failure. `cancel` and `modify` return void.

| Function | Description |
|----------|-------------|
| `lrvx_emit_market_buy(s, sym, qty_raw)` | Market buy |
| `lrvx_emit_market_sell(s, sym, qty_raw)` | Market sell |
| `lrvx_emit_limit_buy(s, sym, px_raw, qty_raw)` | Limit buy |
| `lrvx_emit_limit_sell(s, sym, px_raw, qty_raw)` | Limit sell |
| `lrvx_emit_limit_buy_tif(s, sym, px_raw, qty_raw, tif)` | Limit buy with time-in-force |
| `lrvx_emit_limit_sell_tif(s, sym, px_raw, qty_raw, tif)` | Limit sell with time-in-force |
| `lrvx_emit_stop_market(s, sym, side, trigger_raw, qty_raw)` | Stop market |
| `lrvx_emit_stop_limit(s, sym, side, trigger_raw, limit_raw, qty_raw)` | Stop limit |
| `lrvx_emit_take_profit_market(s, sym, side, trigger_raw, qty_raw)` | Take-profit market |
| `lrvx_emit_take_profit_limit(s, sym, side, trigger_raw, limit_raw, qty_raw)` | Take-profit limit |
| `lrvx_emit_trailing_stop(s, sym, side, offset_raw, qty_raw)` | Trailing stop (absolute) |
| `lrvx_emit_trailing_stop_percent(s, sym, side, bps, qty_raw)` | Trailing stop (basis points) |
| `lrvx_emit_close_position(s, sym)` | Close position (reduce-only) |
| `lrvx_emit_cancel(s, order_id)` | Cancel order |
| `lrvx_emit_cancel_all(s, sym)` | Cancel all orders for symbol |
| `lrvx_emit_modify(s, order_id, new_price_raw, new_qty_raw)` | Modify order |

---

## Context queries

| Function | Returns | Description |
|----------|---------|-------------|
| `lrvx_position_raw(s, sym)` | `int64_t` | Position × 1e8 |
| `lrvx_last_trade_price_raw(s, sym)` | `int64_t` | Last trade price × 1e8 |
| `lrvx_best_bid_raw(s, sym)` | `int64_t` | Best bid × 1e8, 0 if none |
| `lrvx_best_ask_raw(s, sym)` | `int64_t` | Best ask × 1e8, 0 if none |
| `lrvx_mid_price_raw(s, sym)` | `int64_t` | Mid price × 1e8, 0 if none |
| `lrvx_best_bid_raw_opt(s, sym, price_out)` | `uint8_t` | 1 with the raw bid in `price_out`, 0 if none |
| `lrvx_best_ask_raw_opt(s, sym, price_out)` | `uint8_t` | 1 with the raw ask in `price_out`, 0 if none |
| `lrvx_mid_price_raw_opt(s, sym, price_out)` | `uint8_t` | 1 with the raw mid in `price_out`, 0 if none |
| `lrvx_get_symbol_context(s, sym, out)` | `void` | Fill `LrvxSymbolContext` |
| `lrvx_get_order_status(s, order_id)` | `int32_t` | Order status (-1 = not found) |

### No quote, and prices at or below zero

A price below zero is a quote: WTI settled at -37.63 in April 2020, day-ahead
power clears below zero on a windy afternoon, and a calendar spread is negative
in contango. A price of exactly 0.0 is a quote too. The book reports such a
book the same way it reports any other, so a return value cannot double as a
"no quote" marker.

The three `int64_t` accessors return 0 for an empty side and for a best quote
of 0.0 alike; they are unchanged, for callers whose markets never reach zero.
The `_opt` trio puts the answer in the return value instead -- 1 with the raw
price written to `price_out`, 0 with `price_out` untouched -- so the two cases
are distinct. `price_out` may be `NULL` when only the flag is wanted. A header
carrying them defines `LRVX_HAS_OPTIONAL_RAW_BEST_QUOTE`, so a binding can
compile against either.

`LrvxSymbolContext.book` has the same limitation as the `int64_t` accessors:
`bid_price_raw`, `ask_price_raw`, `mid_raw` and `spread_raw` are 0 when the
side is empty. Read the `_opt` accessors where that matters.

The book handle accessors -- `lrvx_book_best_bid`, `lrvx_book_best_ask`,
`lrvx_book_mid`, `lrvx_book_spread` -- already return a presence flag with the
price in an out parameter and need no variant; they report a negative best
quote like any other.

---

## Simulated executor

Used in backtesting to fill orders from simulated market data.

```c
LrvxSimulatedExecutorHandle lrvx_simulated_executor_create(void);
void               lrvx_simulated_executor_destroy(LrvxSimulatedExecutorHandle executor);

void lrvx_simulated_executor_submit_order(LrvxSimulatedExecutorHandle executor,
                                uint64_t id, uint8_t side, double price,
                                double quantity, uint8_t order_type, uint32_t symbol);
void lrvx_simulated_executor_cancel_order(LrvxSimulatedExecutorHandle executor, uint64_t order_id);
void lrvx_simulated_executor_cancel_all(LrvxSimulatedExecutorHandle executor, uint32_t symbol);

// Feed market data
void lrvx_simulated_executor_on_bar(LrvxSimulatedExecutorHandle executor, uint32_t symbol, double close_price);
void lrvx_simulated_executor_on_trade(LrvxSimulatedExecutorHandle executor, uint32_t symbol,
                            double price, uint8_t is_buy);
void lrvx_simulated_executor_on_trade_qty(LrvxSimulatedExecutorHandle executor, uint32_t symbol,
                                double price, double quantity, uint8_t is_buy);
void lrvx_simulated_executor_on_best_levels(LrvxSimulatedExecutorHandle executor, uint32_t symbol,
                                  double bid_price, double bid_qty,
                                  double ask_price, double ask_qty);
void lrvx_simulated_executor_on_book_snapshot(LrvxSimulatedExecutorHandle executor, uint32_t symbol,
                                    const double* bid_prices, const double* bid_qtys,
                                    uint32_t n_bids,
                                    const double* ask_prices, const double* ask_qtys,
                                    uint32_t n_asks);
void lrvx_simulated_executor_advance_clock(LrvxSimulatedExecutorHandle executor, int64_t timestamp_ns);

// Fills
uint32_t lrvx_simulated_executor_fill_count(LrvxSimulatedExecutorHandle executor);
uint32_t lrvx_simulated_executor_get_fills(LrvxSimulatedExecutorHandle executor,
                                 LrvxFill* fills_out, uint32_t max_fills);
```

### Slippage configuration

```c
typedef enum {
    LRVX_SLIPPAGE_NONE         = 0,
    LRVX_SLIPPAGE_FIXED_TICKS  = 1,
    LRVX_SLIPPAGE_FIXED_BPS    = 2,
    LRVX_SLIPPAGE_VOLUME_IMPACT = 3
} LrvxSlippageModel;

void lrvx_simulated_executor_set_default_slippage(LrvxSimulatedExecutorHandle executor,
                                        int32_t model, int32_t ticks,
                                        double tick_size, double bps,
                                        double impact_coeff);
void lrvx_simulated_executor_set_symbol_slippage(LrvxSimulatedExecutorHandle executor, uint32_t symbol,
                                       int32_t model, int32_t ticks,
                                       double tick_size, double bps,
                                       double impact_coeff);
```

### Queue simulation

```c
typedef enum {
    LRVX_QUEUE_NONE                = 0,
    LRVX_QUEUE_TOB                 = 1,
    LRVX_QUEUE_FULL                = 2,
    LRVX_QUEUE_PRO_RATA            = 3,
    LRVX_QUEUE_PRO_RATA_WITH_FIFO  = 4
} LrvxQueueModel;

void lrvx_simulated_executor_set_queue_model(LrvxSimulatedExecutorHandle executor,
                                   int32_t model, uint32_t depth);
```

The C++ `QueueModel` enum has two further values (`TOP_PRO_LMM`, `PRO_RATA_WITH_PRIORITY`) with no
C-API constant; see [queue simulation](../backtest/queue_simulation.md).

### Aggregator event filter

```c
typedef enum {
    LRVX_AGG_FILTER_TRADES     = 1,
    LRVX_AGG_FILTER_BOOKS_ONLY = 2,
    LRVX_AGG_FILTER_BOTH       = 3
} LrvxAggregatorEventFilter;
```

Selects which tape events feed a `LrvxAggregatorHandle`. These three enums (`LrvxSlippageModel`,
`LrvxQueueModel`, `LrvxAggregatorEventFilter`) are the only C enums in the header; every other
discrete value crosses the boundary as a bare `uint8_t` or `int32_t`.

---

## BacktestResult

Aggregates fills into trades, statistics, and equity curve.

```c
LrvxBacktestResultHandle lrvx_backtest_result_create(double initial_capital,
                                                     double fee_rate,
                                                     uint8_t use_percentage_fee,
                                                     double fixed_fee_per_trade,
                                                     double risk_free_rate,
                                                     double annualization_factor);
void lrvx_backtest_result_destroy(LrvxBacktestResultHandle result);

void lrvx_backtest_result_record_fill(LrvxBacktestResultHandle result,
                                      uint64_t order_id, uint32_t symbol, uint8_t side,
                                      double price, double quantity, int64_t timestamp_ns);
void lrvx_backtest_result_ingest_executor(LrvxBacktestResultHandle result,
                                          LrvxSimulatedExecutorHandle executor);

void     lrvx_backtest_result_stats(LrvxBacktestResultHandle result, LrvxBacktestStats* out);
uint32_t lrvx_backtest_result_equity_curve(LrvxBacktestResultHandle result,
                                           LrvxEquityPoint* points_out, uint32_t max_points);
uint8_t  lrvx_backtest_result_write_equity_curve_csv(LrvxBacktestResultHandle result,
                                                     const char* path);
```

---

## Indicators

Stateless, array-in / array-out.

| Function | Description |
|----------|-------------|
| `lrvx_indicator_ema(input, len, period, output)` | EMA |
| `lrvx_indicator_sma(input, len, period, output)` | SMA |
| `lrvx_indicator_rsi(input, len, period, output)` | RSI |
| `lrvx_indicator_rma(input, len, period, output)` | Wilder's moving average |
| `lrvx_indicator_dema(input, len, period, output)` | Double EMA |
| `lrvx_indicator_tema(input, len, period, output)` | Triple EMA |
| `lrvx_indicator_kama(input, len, period, fast, slow, output)` | Kaufman adaptive MA |
| `lrvx_indicator_slope(input, len, length, output)` | Linear slope |
| `lrvx_indicator_atr(high, low, close, len, period, output)` | ATR |
| `lrvx_indicator_adx(high, low, close, len, period, adx, +di, -di)` | ADX |
| `lrvx_indicator_macd(input, len, fast, slow, signal, macd, signal, hist)` | MACD |
| `lrvx_indicator_bollinger(input, len, period, mult, upper, middle, lower)` | Bollinger Bands |
| `lrvx_indicator_cci(high, low, close, len, period, output)` | CCI |
| `lrvx_indicator_stochastic(high, low, close, len, k, d, k_out, d_out)` | Stochastic |
| `lrvx_indicator_chop(high, low, close, len, period, output)` | Choppiness |
| `lrvx_indicator_obv(close, volume, len, output)` | On-balance volume |
| `lrvx_indicator_vwap(close, volume, len, window, output)` | Rolling VWAP |
| `lrvx_indicator_cvd(open, high, low, close, volume, len, output)` | Cumulative volume delta |
| `lrvx_indicator_skewness(input, len, period, output)` | Rolling skewness |
| `lrvx_indicator_kurtosis(input, len, period, output)` | Rolling kurtosis |
| `lrvx_indicator_parkinson_vol(high, low, len, period, output)` | Parkinson volatility |
| `lrvx_indicator_rogers_satchell_vol(open, high, low, close, len, period, output)` | Rogers-Satchell volatility |
| `lrvx_indicator_rolling_zscore(input, len, period, output)` | Rolling z-score |
| `lrvx_indicator_shannon_entropy(input, len, period, bins, output)` | Shannon entropy |
| `lrvx_indicator_correlation(x, y, len, period, output)` | Rolling correlation |
| `lrvx_indicator_adf(input, len, max_lag, regression, test_stat_out, p_value_out, used_lag_out)` | Augmented Dickey-Fuller test |
| `lrvx_indicator_autocorrelation(input, len, window, lag, output)` | Rolling autocorrelation |

That is all 27 functions in the `indicator_functions` group. `lrvx_indicator_adf` is the only one
that is not array-in / array-out: it writes three scalars through out-pointers and takes a
`const char* regression` selector. The `lrvx_indicator_graph_*` functions are a separate group and
are not listed here.

Streaming (stateful) indicator objects are not part of the C API; they are exposed by the Codon,
QuickJS and Node bindings.

---

## Bar aggregation

All functions return the total number of bars the input produced, and write as many of them
as fit into `bars_out` (capacity `max`). Time, Tick, Volume, Range, and Heikin-Ashi close at
most one bar per input trade, so sizing `bars_out` to `len` is always enough for them. Renko is
the exception: a single trade that gaps past more than one brick width closes the brick that
was forming and also synthesizes the bricks in between (see [bar types](../../../explanation/bar-types.md#renko-bars)),
so it can return more bars than there were input trades. If the return value is greater than
`max`, only the first `max` bars were written -- call again with a buffer sized to the return
value to get the rest.

| Function | Description |
|----------|-------------|
| `lrvx_aggregate_time_bars(..., interval_seconds, bars_out, max)` | Time bars |
| `lrvx_aggregate_tick_bars(..., tick_count, bars_out, max)` | Tick bars |
| `lrvx_aggregate_volume_bars(..., volume_threshold, bars_out, max)` | Volume bars |
| `lrvx_aggregate_range_bars(..., range_size, bars_out, max)` | Range bars |
| `lrvx_aggregate_renko_bars(..., brick_size, bars_out, max)` | Renko bars |
| `lrvx_aggregate_heikin_ashi_bars(..., interval_seconds, bars_out, max)` | Heikin-Ashi |

All take the same input signature: `(timestamps, prices, quantities, is_buy, len, ...)`.

---

## L2 Order book

```c
LrvxBookHandle lrvx_book_create(double tick_size);
void           lrvx_book_destroy(LrvxBookHandle book);

void    lrvx_book_apply_snapshot(LrvxBookHandle book,
                                 const double* bid_prices, const double* bid_qtys, size_t bid_len,
                                 const double* ask_prices, const double* ask_qtys, size_t ask_len);
void    lrvx_book_apply_delta(LrvxBookHandle book,
                              const double* bid_prices, const double* bid_qtys, size_t bid_len,
                              const double* ask_prices, const double* ask_qtys, size_t ask_len);

uint8_t lrvx_book_best_bid(LrvxBookHandle book, double* price_out);
uint8_t lrvx_book_best_ask(LrvxBookHandle book, double* price_out);
uint8_t lrvx_book_mid(LrvxBookHandle book, double* price_out);
uint8_t lrvx_book_spread(LrvxBookHandle book, double* spread_out);
double  lrvx_book_bid_at_price(LrvxBookHandle book, double price);
double  lrvx_book_ask_at_price(LrvxBookHandle book, double price);
uint8_t lrvx_book_is_crossed(LrvxBookHandle book);
void    lrvx_book_clear(LrvxBookHandle book);

uint32_t lrvx_book_get_bids(LrvxBookHandle book, double* prices_out,
                            double* qtys_out, uint32_t max_levels);
uint32_t lrvx_book_get_asks(LrvxBookHandle book, double* prices_out,
                            double* qtys_out, uint32_t max_levels);
```

---

## L3 Order book

```c
LrvxL3BookHandle lrvx_l3_book_create(void);
void             lrvx_l3_book_destroy(LrvxL3BookHandle book);

int32_t lrvx_l3_book_add_order(LrvxL3BookHandle book,
                               uint64_t order_id, double price,
                               double quantity, uint8_t side);
int32_t lrvx_l3_book_remove_order(LrvxL3BookHandle book, uint64_t order_id);
int32_t lrvx_l3_book_modify_order(LrvxL3BookHandle book,
                                  uint64_t order_id, double new_qty);

uint8_t lrvx_l3_book_best_bid(LrvxL3BookHandle book, double* price_out);
uint8_t lrvx_l3_book_best_ask(LrvxL3BookHandle book, double* price_out);
double  lrvx_l3_book_bid_at_price(LrvxL3BookHandle book, double price);
double  lrvx_l3_book_ask_at_price(LrvxL3BookHandle book, double price);
```

---

## Composite book

Aggregates books across multiple exchanges per symbol.

```c
LrvxCompositeBookHandle lrvx_composite_book_create(void);
void                    lrvx_composite_book_destroy(LrvxCompositeBookHandle book);

uint8_t lrvx_composite_book_best_bid(LrvxCompositeBookHandle book, uint32_t symbol,
                                     double* price_out, double* qty_out);
uint8_t lrvx_composite_book_best_ask(LrvxCompositeBookHandle book, uint32_t symbol,
                                     double* price_out, double* qty_out);
uint8_t lrvx_composite_book_has_arb(LrvxCompositeBookHandle book, uint32_t symbol);
void    lrvx_composite_book_mark_stale(LrvxCompositeBookHandle book,
                                       uint32_t exchange, uint32_t symbol);
void    lrvx_composite_book_check_staleness(LrvxCompositeBookHandle book,
                                            int64_t now_ns, int64_t threshold_ns);
```

---

## Position tracker

FIFO/average cost position tracking.

```c
LrvxPositionTrackerHandle lrvx_position_tracker_create(uint8_t cost_basis); // 0 = FIFO
void                      lrvx_position_tracker_destroy(LrvxPositionTrackerHandle tracker);

void   lrvx_position_tracker_on_fill(LrvxPositionTrackerHandle tracker,
                                     uint32_t symbol, uint8_t side,
                                     double price, double quantity);
double lrvx_position_tracker_position(LrvxPositionTrackerHandle tracker, uint32_t symbol);
double lrvx_position_tracker_avg_entry(LrvxPositionTrackerHandle tracker, uint32_t symbol);
double lrvx_position_tracker_realized_pnl(LrvxPositionTrackerHandle tracker, uint32_t symbol);
double lrvx_position_tracker_total_pnl(LrvxPositionTrackerHandle tracker);
```

---

## Position group

Tracks individual named positions (open/partial-close/close).

```c
LrvxPositionGroupHandle lrvx_position_group_create(void);
void                    lrvx_position_group_destroy(LrvxPositionGroupHandle tracker);

uint64_t lrvx_position_group_open(LrvxPositionGroupHandle tracker,
                                  uint64_t order_id, uint32_t symbol,
                                  uint8_t side, double price, double qty);
void     lrvx_position_group_close(LrvxPositionGroupHandle tracker,
                                   uint64_t position_id, double exit_price);
void     lrvx_position_group_partial_close(LrvxPositionGroupHandle tracker,
                                           uint64_t position_id,
                                           double qty, double exit_price);

double   lrvx_position_group_net_position(LrvxPositionGroupHandle tracker, uint32_t symbol);
double   lrvx_position_group_realized_pnl(LrvxPositionGroupHandle tracker, uint32_t symbol);
double   lrvx_position_group_total_pnl(LrvxPositionGroupHandle tracker);
uint32_t lrvx_position_group_open_count(LrvxPositionGroupHandle tracker, uint32_t symbol);
void     lrvx_position_group_prune(LrvxPositionGroupHandle tracker);
```

---

## Order tracker

Tracks submitted/filled/canceled orders.

```c
LrvxOrderTrackerHandle lrvx_order_tracker_create(void);
void                   lrvx_order_tracker_destroy(LrvxOrderTrackerHandle tracker);

uint8_t  lrvx_order_tracker_on_submitted(LrvxOrderTrackerHandle tracker,
                                         uint64_t order_id, uint32_t symbol,
                                         uint8_t side, double price, double qty);
uint8_t  lrvx_order_tracker_on_filled(LrvxOrderTrackerHandle tracker,
                                      uint64_t order_id, double fill_qty);
uint8_t  lrvx_order_tracker_on_canceled(LrvxOrderTrackerHandle tracker, uint64_t order_id);
uint8_t  lrvx_order_tracker_is_active(LrvxOrderTrackerHandle tracker, uint64_t order_id);
uint32_t lrvx_order_tracker_active_count(LrvxOrderTrackerHandle tracker);
uint32_t lrvx_order_tracker_total_count(LrvxOrderTrackerHandle tracker);
void     lrvx_order_tracker_prune(LrvxOrderTrackerHandle tracker);
```

---

## Volume profile

```c
LrvxVolumeProfileHandle lrvx_volume_profile_create(double tick_size);
void                    lrvx_volume_profile_destroy(LrvxVolumeProfileHandle profile);

void     lrvx_volume_profile_add_trade(LrvxVolumeProfileHandle profile,
                                       double price, double quantity, uint8_t is_buy);
double   lrvx_volume_profile_poc(LrvxVolumeProfileHandle profile);
double   lrvx_volume_profile_vah(LrvxVolumeProfileHandle profile);
double   lrvx_volume_profile_val(LrvxVolumeProfileHandle profile);
double   lrvx_volume_profile_total_volume(LrvxVolumeProfileHandle profile);
double   lrvx_volume_profile_total_delta(LrvxVolumeProfileHandle profile);
uint32_t lrvx_volume_profile_num_levels(LrvxVolumeProfileHandle profile);
void     lrvx_volume_profile_clear(LrvxVolumeProfileHandle profile);
```

---

## Market profile

Tracks TPO-style market profile with initial balance.

```c
LrvxMarketProfileHandle lrvx_market_profile_create(double tick_size,
                                                   uint32_t period_minutes,
                                                   int64_t session_start_ns);
void lrvx_market_profile_destroy(LrvxMarketProfileHandle profile);

void     lrvx_market_profile_add_trade(LrvxMarketProfileHandle profile,
                                       int64_t timestamp_ns, double price,
                                       double qty, uint8_t is_buy);
double   lrvx_market_profile_poc(LrvxMarketProfileHandle profile);
double   lrvx_market_profile_vah(LrvxMarketProfileHandle profile);
double   lrvx_market_profile_val(LrvxMarketProfileHandle profile);
double   lrvx_market_profile_ib_high(LrvxMarketProfileHandle profile);
double   lrvx_market_profile_ib_low(LrvxMarketProfileHandle profile);
uint8_t  lrvx_market_profile_is_poor_high(LrvxMarketProfileHandle profile);
uint8_t  lrvx_market_profile_is_poor_low(LrvxMarketProfileHandle profile);
uint32_t lrvx_market_profile_num_levels(LrvxMarketProfileHandle profile);
void     lrvx_market_profile_clear(LrvxMarketProfileHandle profile);
```

---

## Footprint

Per-price buy/sell delta at bar resolution.

```c
LrvxFootprintHandle lrvx_footprint_create(double tick_size);
void                lrvx_footprint_destroy(LrvxFootprintHandle footprint);

void     lrvx_footprint_add_trade(LrvxFootprintHandle footprint,
                                  double price, double quantity, uint8_t is_buy);
double   lrvx_footprint_total_delta(LrvxFootprintHandle footprint);
double   lrvx_footprint_total_volume(LrvxFootprintHandle footprint);
uint32_t lrvx_footprint_num_levels(LrvxFootprintHandle footprint);
void     lrvx_footprint_clear(LrvxFootprintHandle footprint);
```

---

## Statistics

| Function | Returns | Description |
|----------|---------|-------------|
| `lrvx_stat_correlation(x, y, len)` | `double` | Pearson correlation |
| `lrvx_stat_profit_factor(pnl, len)` | `double` | Gross profit / gross loss |
| `lrvx_stat_win_rate(pnl, len)` | `double` | Winning trade ratio |
| `lrvx_stat_permutation_test(g1, l1, g2, l2, n)` | `double` | Two-sample permutation p-value |
| `lrvx_stat_bootstrap_ci(data, len, conf, n, &lo, &med, &hi)` | `void` | Bootstrap confidence interval |

---

## Data writer

Writes trades and book updates to binary log segments.

```c
LrvxDataWriterHandle lrvx_data_writer_create(const char* output_dir,
                                             uint64_t max_segment_mb,
                                             uint8_t exchange_id);
void lrvx_data_writer_destroy(LrvxDataWriterHandle writer);

uint8_t lrvx_data_writer_write_trade(LrvxDataWriterHandle writer,
                                     int64_t exchange_ts_ns, int64_t recv_ts_ns,
                                     double price, double qty,
                                     uint64_t trade_id, uint32_t symbol_id, uint8_t side);

// Raw int64 book levels (scale 1e8). bids/asks may be NULL when the
// matching count is 0. Returns 1 on success, 0 on failure.
uint8_t lrvx_data_writer_write_book(LrvxDataWriterHandle writer,
                                    int64_t exchange_ts_ns, int64_t recv_ts_ns,
                                    int64_t seq, uint32_t symbol_id,
                                    uint8_t is_snapshot,
                                    const LrvxBookLevel* bids, uint32_t n_bids,
                                    const LrvxBookLevel* asks, uint32_t n_asks);

// Batched book writer. headers + flat levels array, sliced per event
// via header.level_offset / bid_count / ask_count. Same struct layout
// as lrvx_data_reader_read_book_updates — round-trip works.
uint64_t lrvx_data_writer_write_books(LrvxDataWriterHandle writer,
                                      const LrvxBookUpdateHeader* headers,
                                      uint64_t n_events,
                                      const LrvxLevel* levels,
                                      uint64_t total_levels);

void lrvx_data_writer_flush(LrvxDataWriterHandle writer);
void lrvx_data_writer_close(LrvxDataWriterHandle writer);
void lrvx_data_writer_stats_p(LrvxDataWriterHandle writer, void* out); // → LrvxWriterStats
```

---

## Data reader

Reads binary log segments.

```c
LrvxDataReaderHandle lrvx_data_reader_create(const char* data_dir);
LrvxDataReaderHandle lrvx_data_reader_create_filtered(const char* data_dir,
                                                       int64_t from_ns, int64_t to_ns,
                                                       const uint32_t* symbols,
                                                       uint32_t num_symbols);
void lrvx_data_reader_destroy(LrvxDataReaderHandle reader);

uint64_t lrvx_data_reader_count(LrvxDataReaderHandle reader);
void     lrvx_data_reader_summary_p(LrvxDataReaderHandle reader, void* out); // → LrvxDatasetSummary
void     lrvx_data_reader_stats_p(LrvxDataReaderHandle reader, void* out);   // → LrvxReaderStats

// Returns number of trades read. If trades_out is NULL, counts only.
uint64_t lrvx_data_reader_read_trades(LrvxDataReaderHandle reader,
                                      LrvxTradeRecord* trades_out, uint64_t max_trades);

// Top-of-book per book update event. If bbos_out is NULL, counts only.
uint64_t lrvx_data_reader_read_bbo(LrvxDataReaderHandle reader,
                                   LrvxBBO* bbos_out, uint64_t max_events);

// Counts events and total levels in one pass. *total_levels_out may be NULL.
uint64_t lrvx_data_reader_count_book_updates(LrvxDataReaderHandle reader,
                                             uint64_t* total_levels_out);

// Reads book updates into pre-sized headers and a single flat levels array.
// Caller sizes both via lrvx_data_reader_count_book_updates() first.
// Each header carries level_offset, bid_count, ask_count for slicing the
// levels array. Bids are written before asks for each event.
uint64_t lrvx_data_reader_read_book_updates(LrvxDataReaderHandle reader,
                                            LrvxBookUpdateHeader* headers_out,
                                            uint64_t max_events,
                                            LrvxLevel* levels_out,
                                            uint64_t max_levels);
```

`LrvxTradeRecord` fields: `exchange_ts_ns`, `recv_ts_ns`, `price_raw`, `qty_raw`, `trade_id`, `symbol_id`, `side`.

`LrvxBBO` fields (size: 64 B): `exchange_ts_ns`, `recv_ts_ns`, `seq`, `bid_price_raw`, `bid_qty_raw`, `ask_price_raw`, `ask_qty_raw`, `symbol_id`, `event_type` (2=snapshot, 3=delta).

`LrvxBookUpdateHeader` fields (size: 48 B): `exchange_ts_ns`, `recv_ts_ns`, `seq`, `level_offset`, `symbol_id`, `bid_count`, `ask_count`, `event_type`.

`LrvxLevel` fields (size: 24 B): `price_raw`, `qty_raw`, `side` (0=bid, 1=ask).

Layout sizes are pinned with `static_assert`; language bindings (Codon, QuickJS) parse these structs from raw byte buffers and depend on exact offsets.

Live segments are safe to read while a writer is still appending. Compressed segments whose header has not yet been finalized (`event_count == 0`) are recovered by walking block headers and decompressing the first / last viable block; the very last block is often truncated, so the scan iterates backwards until one decompresses successfully. The same recovery is used by `summary()` / `inspect()`.

---

## Binary-log recorder hook

Built-in `.lrvx` sink. Owns a `BinaryLogWriter` and routes runner /
live-engine events into it on the C++ side, without crossing into the
host language per event. `as_recorder` yields a borrowed handle for
`lrvx_runner_set_market_data_recorder` /
`lrvx_live_engine_set_market_data_recorder`.

```c
LrvxBinaryLogRecorderHookHandle
lrvx_binary_log_recorder_hook_create(const char* output_dir,
                                     uint64_t max_segment_mb,
                                     uint8_t exchange_id,
                                     uint8_t compression /* 0=None, 1=LZ4 */);
void lrvx_binary_log_recorder_hook_destroy(LrvxBinaryLogRecorderHookHandle hook);

LrvxMarketDataRecorderHandle
lrvx_binary_log_recorder_hook_as_recorder(LrvxBinaryLogRecorderHookHandle hook);

void lrvx_binary_log_recorder_hook_add_symbol(LrvxBinaryLogRecorderHookHandle hook,
                                              uint32_t symbol_id, const char* name,
                                              const char* base, const char* quote,
                                              int8_t price_precision,
                                              int8_t qty_precision);

void lrvx_binary_log_recorder_hook_flush(LrvxBinaryLogRecorderHookHandle hook);
LrvxWriterStats lrvx_binary_log_recorder_hook_stats(LrvxBinaryLogRecorderHookHandle hook);
void lrvx_binary_log_recorder_hook_stats_p(void* hook, void* out);
```

---

## Segment operations

```c
// Quick validate/merge
uint8_t lrvx_segment_validate(const char* path);
uint8_t lrvx_segment_merge(const char* input_dir, const char* output_path);

// Full API (results written to out pointer, see struct definitions in header)
void lrvx_segment_merge_full_p(const char* input_paths, size_t num_paths,
                                const char* output_dir, const char* output_name,
                                uint8_t sort, void* out);         // → LrvxMergeResult
void lrvx_segment_merge_dir_p(const char* input_dir,
                               const char* output_dir, void* out); // → LrvxMergeResult
void lrvx_segment_split_p(const char* input_path, const char* output_dir,
                           uint8_t mode, int64_t time_interval_ns,
                           uint64_t events_per_file, void* out);   // → LrvxSplitResult
void lrvx_segment_export_p(const char* input_path, const char* output_path,
                            uint8_t format, int64_t from_ns, int64_t to_ns,
                            const uint32_t* symbols, uint32_t num_symbols,
                            void* out);                            // → LrvxExportResult

uint8_t  lrvx_segment_recompress(const char* input_path, const char* output_path,
                                 uint8_t compression);
uint64_t lrvx_segment_extract_symbols(const char* input_path, const char* output_path,
                                      const uint32_t* symbols, uint32_t num_symbols);
uint64_t lrvx_segment_extract_time_range(const char* input_path, const char* output_path,
                                         int64_t from_ns, int64_t to_ns);

// Validation
void lrvx_segment_validate_full_p(const char* path, uint8_t verify_crc,
                                   uint8_t verify_timestamps, void* out); // → LrvxSegmentValidation
void lrvx_dataset_validate_p(const char* data_dir, void* out);            // → LrvxDatasetValidation
```

---

## Partitioner

Splits a dataset into time or event-count partitions for parallel backtesting.

```c
LrvxPartitionerHandle lrvx_partitioner_create(const char* data_dir);
void                  lrvx_partitioner_destroy(LrvxPartitionerHandle partitioner);

// All return number of partitions. If partitions_out is NULL, counts only.
uint32_t lrvx_partitioner_by_time(LrvxPartitionerHandle p, uint32_t num_partitions,
                                   int64_t warmup_ns,
                                   LrvxPartition* partitions_out, uint32_t max);
uint32_t lrvx_partitioner_by_duration(LrvxPartitionerHandle p, int64_t duration_ns,
                                       int64_t warmup_ns,
                                       LrvxPartition* partitions_out, uint32_t max);
uint32_t lrvx_partitioner_by_calendar(LrvxPartitionerHandle p, uint8_t unit,
                                       int64_t warmup_ns,
                                       LrvxPartition* partitions_out, uint32_t max);
uint32_t lrvx_partitioner_by_symbol(LrvxPartitionerHandle p, uint32_t num_partitions,
                                     LrvxPartition* partitions_out, uint32_t max);
uint32_t lrvx_partitioner_per_symbol(LrvxPartitionerHandle p,
                                      LrvxPartition* partitions_out, uint32_t max);
uint32_t lrvx_partitioner_by_event_count(LrvxPartitionerHandle p, uint32_t num_partitions,
                                          LrvxPartition* partitions_out, uint32_t max);
```

`LrvxPartition` fields: `partition_id`, `from_ns`, `to_ns`, `warmup_from_ns`, `estimated_events`, `estimated_bytes`.

---

## Fixed-point conversion

```c
int64_t lrvx_price_from_double(double value);
double  lrvx_price_to_double(int64_t raw);
int64_t lrvx_quantity_from_double(double value);
double  lrvx_quantity_to_double(int64_t raw);
```

Scale factor is 1e8 for both price and quantity.
