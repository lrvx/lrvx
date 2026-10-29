// TypeScript definitions for lrvx.
//
// Hand-written from the N-API binding in node/src/. Kept in sync with the
// C++ surface by:
//   - scripts/check_dts_exports.py   (source-level: every `exports.Set("X", ...)`
//                                     in node/src/*.h has a matching declaration)
//   - node/test/test_types.ts        (signature-level: tsc --noEmit)
// Both run in CI (linux-gcc job).

// ── Errors ────────────────────────────────────────────────────────────

/**
 * Structured lrvx error. Thrown by Engine / BacktestRunner / etc. when
 * a stable error code is appropriate. Compatible with `instanceof Error`.
 *
 * The `code` follows the convention `E_<DOMAIN>_<NNN>` (e.g. `E_IO_001`,
 * `E_RUN_002`). The `helpUrl` points to the canonical fix-recipe page on
 * the lrvx docs site.
 *
 * Example:
 *
 * ```js
 * try {
 *   engine.loadCsv("missing.csv", "BTC");
 * } catch (e) {
 *   if (e.code === "E_IO_001") console.log(`see ${e.helpUrl}`);
 *   throw e;
 * }
 * ```
 */
export interface LrvxError extends Error {
  readonly code: string;
  readonly helpUrl: string;
}

// ── C ABI version ─────────────────────────────────────────────────────

/**
 * C ABI version this addon was compiled against. The addon compares it
 * against {@link capiAbiVersion} when it loads and throws from
 * `require` when the two disagree -- the structs on that boundary carry
 * no reserved tail, so a skew reads as wrong numbers rather than a
 * failed load.
 */
export const CAPI_ABI_VERSION: number;

/** C ABI version the loaded lrvx library reports. */
export function capiAbiVersion(): number;

// ── Common types ──────────────────────────────────────────────────────

/** Side of an order or trade. */
export type Side = "buy" | "sell";

/** Order type accepted by the executors. */
export type OrderType =
  | "market"
  | "limit"
  | "stop_market"
  | "stop_limit"
  | "trailing_stop";

/** Slippage model name accepted by `SimulatedExecutor.setDefaultSlippage`. */
export type SlippageModel =
  | "none"
  | "fixed_ticks"
  | "fixed_bps"
  | "volume_impact";

/** Limit-order queue model name accepted by `SimulatedExecutor.setQueueModel`. */
export type QueueModel =
  | "none"
  | "tob"
  | "full"
  | "pro_rata"
  | "pro_rata_with_fifo"
  | "top_pro_lmm"
  | "pro_rata_with_priority";

/** Position cost-basis mode accepted by `PositionTracker`. */
export type PositionMode = 0 | 1;

/** OHLCV bar emitted by the C++ aggregators / fed via `Runner.onBar`. */
export interface BarData {
  open: number;
  high: number;
  low: number;
  close: number;
  volume: number;
  buyVolume: number;
  /** Absolute nanosecond clock readings: a `Number` is a double and steps
   *  256 ns at a time at present-day magnitudes. */
  startTimeNs: bigint;
  endTimeNs: bigint;
  /** 0=Time, 1=Tick, 2=Volume, 3=Renko, 4=Range, 5=HeikinAshi, 6=BpsRange.
   *  Mirrors `lrvx::BarType` (include/lrvx/aggregator/bar.h) and the
   *  `composite.BAR_TYPE_*` constants. */
  barType: number;
  /** Interval / threshold for the active bar policy. */
  barTypeParam: number;
  /** 0=Threshold, 1=Gap, 2=Forced, 3=Warmup. The engine reports Threshold
   *  (normal close), Gap (the bar absorbing the remainder of a Renko price
   *  jump too wide to walk brick by brick) or Forced (stop() flush); Warmup
   *  is set by callers, not the engine (see
   *  docs/reference/api/aggregator/bar.md). */
  closeReason: number;
}

/** Per-symbol live context passed to strategy callbacks. */
export interface SymbolContext {
  position: number;
  symbolId: number;
  lastTradePrice: number;
  /** Best bid, or `null` when the bid side has no level. A book may be
   *  quoted at exactly zero, so `0` is a price, never "no quote". */
  bestBid: number | null;
  /** Best ask, or `null` when the ask side has no level. */
  bestAsk: number | null;
  /** Mid price, or `null` unless both sides have a level. */
  midPrice: number | null;
}

/** Trade tick passed to `onTrade`. */
export interface TradeData {
  /** Symbol id. Set on every TradeData the addon delivers. */
  symbol: number;
  price: number;
  qty: number;
  isBuy: boolean;
  side: Side;
  timestampNs: bigint;

  /** @deprecated Alias of `qty`, kept for one release. A
   *  `MarketDataRecorderHook` used to be handed `quantity` instead of the
   *  declared `qty`; both are delivered now. Read `qty`. */
  quantity?: number;
  /** @deprecated Alias of `timestampNs`, kept for one release, and a
   *  `bigint` like it — the reading never fit in a double. A
   *  `MarketDataRecorderHook` used to be handed `exchangeTsNs` instead of
   *  the declared `timestampNs`; both are delivered now. Read
   *  `timestampNs`. */
  exchangeTsNs?: bigint;
}

/** Order-event lifecycle status, mirrored from `LrvxOrderEventStatus`. */
export type OrderEventStatus =
  | "NEW"
  | "ACCEPTED"
  | "PENDING_NEW"
  | "PARTIALLY_FILLED"
  | "FILLED"
  | "PENDING_CANCEL"
  | "CANCELED"
  | "EXPIRED"
  | "REJECTED"
  | "REPLACED"
  | "PENDING_TRIGGER"
  | "TRIGGERED"
  | "TRAILING_UPDATED"
  | "QUEUE_POSITION_UPDATED"
  | "MARKET_POSITION_CHANGED"
  | "REPLACE_SUBMITTED"
  | "REPLACE_ACCEPTED"
  | "REPLACE_REJECTED"
  | "PENDING_ONCHAIN"
  | "REVERTED"
  | "REPLACED_GAS"
  | "UNKNOWN";

/** Categorical position of a resting limit order relative to top-of-book. */
export type MarketPosition =
  | "best"
  | "behind_best"
  | "mid_spread"
  | "level_empty"
  | "crossed";

/** Order-event payload delivered to `onFill` / `onOrderUpdate` /
 *  `onQueuePositionChange`. */
export interface OrderEventData {
  orderId: number;
  symbolId: number;
  side: Side;
  orderType:
    | "LIMIT"
    | "MARKET"
    | "STOP_MARKET"
    | "STOP_LIMIT"
    | "TP_MARKET"
    | "TP_LIMIT"
    | "TRAILING_STOP"
    | "ICEBERG"
    | "UNKNOWN";
  status: OrderEventStatus;
  /** Cumulative or last-fill quantity depending on the executor. */
  fillQty: number;
  /** Last-fill price; 0 for non-fill events. */
  fillPrice: number;
  exchangeTsNs: number;
  /** Set only when status === "REJECTED". */
  rejectReason: string | null;
  /** Backtest only: volume ahead of this order at its price level. */
  queueAhead: number;
  /** Backtest only: total quantity at the order's price level. */
  queueTotal: number;
  /** Per-lifecycle-stage timestamps. Zero means the stage has not
   *  fired yet for this order. */
  submittedAtNs: number;
  acceptedAtNs: number;
  firstFillAtNs: number;
  lastFillAtNs: number;
  canceledAtNs: number;
  rejectedAtNs: number;
  triggeredAtNs: number;
  expiredAtNs: number;
  /** True when the fill came from the order resting in the book and
   *  being consumed by an aggressive opposite trade; false when the
   *  order arrived marketable and crossed the book. Meaningful only
   *  for fill statuses. */
  isMaker: boolean;
  /** "maker" | "taker" for fill statuses; null otherwise. */
  fillRole: "maker" | "taker" | null;
  /** Categorical market position for resting limit orders; null when unknown. */
  marketPosition: MarketPosition | null;
  /** Signed ticks from best on our side. Positive = behind, negative = ahead. */
  distanceToBestTicks: number;
}

/** Closed bar read back from the per-(symbol, timeframe) ring by
 *  `EmitMethods.lastClosedBar` / `lastNClosedBars`. Mirrors `LrvxBar`;
 *  narrower than `BarData` (no `buyVolume` / `barType` / `closeReason`). */
export interface ClosedBar {
  open: number;
  high: number;
  low: number;
  close: number;
  volume: number;
  startNs: bigint;
  endNs: bigint;
}

/** Order-emission helper passed as the third arg to strategy callbacks.
 *  Every `symbol` argument is optional and defaults to the strategy's
 *  first subscribed symbol. */
export interface EmitMethods {
  marketBuy(qty: number, symbol?: Symbol | number): void;
  marketSell(qty: number, symbol?: Symbol | number): void;
  limitBuy(price: number, qty: number, symbol?: Symbol | number): void;
  limitSell(price: number, qty: number, symbol?: Symbol | number): void;
  provideLiquidity(
    priceLower: number,
    priceUpper: number,
    liquidity: number,
    symbol?: Symbol | number,
  ): void;
  withdrawLiquidity(liquidity: number, symbol?: Symbol | number): void;
  cancel(orderId: number): void;
  closePosition(symbol?: Symbol | number): void;
  /** Most recent closed bar for `(symbol, barType, barTypeParam)`, or
   *  `null` when none has closed yet. `barType` uses the `BarData.barType`
   *  encoding; `barTypeParam` is the interval / threshold. */
  lastClosedBar(
    symbol: Symbol | number,
    barType: number,
    barTypeParam: number,
  ): ClosedBar | null;
  /** Up to `n` most recent closed bars, oldest first. */
  lastNClosedBars(
    symbol: Symbol | number,
    barType: number,
    barTypeParam: number,
    n: number,
  ): ClosedBar[];
  /** Ring depth kept per (symbol, timeframe). */
  setBarRingCapacity(n: number): void;
}

/** Backtest signal emitted by `Runner` / collected by `SimulatedExecutor`. */
export interface Signal {
  side: Side;
  quantity: number;
  /** 0 for market orders. */
  price: number;
  orderType: OrderType;
  orderId: number;
  /** Lower bound of the range on a provide/withdraw-liquidity signal, 0 otherwise. */
  rangeLower: number;
  /** Upper bound of the range on a provide-liquidity signal, 0 otherwise. */
  rangeUpper: number;
  /** Position size on a provide/withdraw-liquidity signal, 0 otherwise. */
  liquidity: number;
}

/** User-supplied strategy object. All callbacks are optional. */
export interface Strategy {
  /** Symbols this strategy subscribes to. */
  symbols?: ReadonlyArray<Symbol | number>;
  onStart?(): void;
  onStop?(): void;
  onTrade?(ctx: SymbolContext, trade: TradeData, emit: EmitMethods): void;
  onBookUpdate?(ctx: SymbolContext, emit: EmitMethods): void;
  onBar?(ctx: SymbolContext, bar: BarData, emit: EmitMethods): void;
  /** Fires on every fill the strategy's own orders produce
   *  (status PARTIALLY_FILLED or FILLED). */
  onFill?(ctx: SymbolContext, ev: OrderEventData, emit: EmitMethods): void;
  /** Fires on every order-lifecycle status change: NEW / ACCEPTED /
   *  CANCELED / REJECTED / REPLACED / TRIGGERED / TRAILING_UPDATED.
   *  Includes fills too — pick `onFill` if you only care about those. */
  onOrderUpdate?(ctx: SymbolContext, ev: OrderEventData, emit: EmitMethods): void;
  /** Resting limit order's queue position moved without any other
   *  lifecycle transition. `ev.queueAhead` and `ev.queueTotal` carry
   *  the current snapshot. Backtest only. */
  onQueuePositionChange?(ctx: SymbolContext, ev: OrderEventData, emit: EmitMethods): void;
  /** Resting limit order's categorical market position transitioned
   *  (best / behind_best / mid_spread / level_empty / crossed).
   *  Backtest only. */
  onMarketPositionChange?(ctx: SymbolContext, ev: OrderEventData, emit: EmitMethods): void;
}

// ── Extension hooks ──────────────────────────────────────────────────
//
// All hooks are plain JS objects. Pass to `runner.set<Hook>(obj)` /
// `backtestRunner.set<Hook>(obj)`. Pass `null` to detach.

/** Order snapshot delivered to ExecutionListener / Executor callbacks. */
export interface Order {
  id: number;
  clientOrderId: number;
  symbol: number;
  strategyId: number;
  orderTag: number;
  side: Side;
  orderType: OrderType | "tp_market" | "tp_limit" | "iceberg";
  timeInForce: "gtc" | "ioc" | "fok" | "gtd" | "post_only" | "unknown";
  reduceOnly: boolean;
  postOnly: boolean;
  closePosition: boolean;
  price: number;
  quantity: number;
  filledQuantity: number;
  triggerPrice: number;
  trailingOffset: number;
  /** Absolute nanosecond clock readings: a `Number` is a double and steps
   *  256 ns at present-day magnitudes. */
  createdAtNs: bigint;
  exchangeTsNs: bigint;
}

/** Returned by `Executor.capabilities()` — what the venue supports. */
export interface ExchangeCapabilities {
  stopMarket?: boolean;
  stopLimit?: boolean;
  takeProfitMarket?: boolean;
  takeProfitLimit?: boolean;
  trailingStop?: boolean;
  iceberg?: boolean;
  oco?: boolean;
  gtc?: boolean;
  ioc?: boolean;
  fok?: boolean;
  gtd?: boolean;
  postOnly?: boolean;
  reduceOnly?: boolean;
  closePosition?: boolean;
  batchSubmit?: boolean;
}

/** Post-emission observer of every signal. Fires from a Node background
 *  thread for live engine; from the JS thread for sync runner. */
export interface PnLTracker {
  onSignal?(signal: Signal): void;
}

/** Persist every emitted signal (DB, audit log, etc.). */
export interface StorageSink {
  store?(signal: Signal): void;
}

/** Pre-trade gate: return false to drop. Sync only — engine reads the
 *  return value inline, so this can't be used with `threaded: true`. */
export interface RiskManager {
  allow?(signal: Signal): boolean;
}

/** Pre-trade gate; halts trading when `check` returns false. Sync only. */
export interface KillSwitch {
  check?(signal: Signal): boolean;
}

/** Pre-trade gate; rejects signals that fail validation. Sync only. */
export interface OrderValidator {
  validate?(signal: Signal): boolean;
}

/** Receives every market-data event fed into the engine, for custom
 *  recording (CSV, parquet, custom binary). */
export interface MarketDataRecorderHook {
  onTrade?(trade: TradeData): void;
  /** `bids` / `asks` are flat `BigInt64Array`s: `[price_raw, qty_raw, ...]`,
   *  raw int64 ticks (multiply by 1e-8 to get a double price/qty). */
  onBookUpdate?(
    symbol: number,
    isSnapshot: boolean,
    bids: BigInt64Array,
    asks: BigInt64Array,
    timestampNs: bigint
  ): void;
  onStart?(): void;
  onStop?(): void;
}

/** Replaces the built-in SimulatedExecutor with a binding-supplied one
 *  (real broker, paper-trading bridge, custom simulator). Sync only —
 *  `capabilities()` is queried inline by the engine. */
export interface Executor {
  submit?(order: Order): void;
  cancel?(orderId: number): void;
  cancelAll?(symbol: number): void;
  replace?(oldOrderId: number, newOrder: Order): void;
  submitOco?(order1: Order, order2: Order): void;
  capabilities?(): ExchangeCapabilities;
  onStart?(): void;
  onStop?(): void;
}

/** Observes order lifecycle from SimulatedExecutor / live broker fills. */
export interface ExecutionListener {
  onSubmitted?(order: Order): void;
  onAccepted?(order: Order): void;
  onPartiallyFilled?(order: Order, fillQuantity: number): void;
  onFilled?(order: Order): void;
  onPendingCancel?(order: Order): void;
  onCanceled?(order: Order): void;
  onExpired?(order: Order): void;
  onRejected?(order: Order, reason: string): void;
  onReplaced?(oldOrder: Order, newOrder: Order): void;
  onPendingTrigger?(order: Order): void;
  onTriggered?(order: Order): void;
  onTrailingStopUpdated?(order: Order, newTriggerPrice: number): void;
}

/** Install a Node callable as the global lrvx log sink. Pass null to
 *  detach. Receives `(level: number, msg: string)`; level: 0=info,
 *  1=warn, 2=error. */
export function setLogCallback(
  callback: ((level: number, msg: string) => void) | null
): void;

/** Stats returned by `BacktestResult.stats()` and `Engine.run()`. */
export interface BacktestStats {
  totalTrades: number;
  winningTrades: number;
  losingTrades: number;
  initialCapital: number;
  finalCapital: number;
  netPnl: number;
  totalPnl: number;
  totalFees: number;
  grossProfit: number;
  grossLoss: number;
  maxDrawdown: number;
  maxDrawdownPct: number;
  winRate: number;
  profitFactor: number;
  avgWin: number;
  avgLoss: number;
  sharpeRatio: number;
  sortinoRatio: number;
  calmarRatio: number;
  returnPct: number;
}

/** Stats returned by every `BacktestRunner.run*` method. `BacktestStats`
 *  minus `totalPnl` / `grossProfit` / `grossLoss`. The risk ratios are
 *  named the same across every producer. */
export interface RunnerStats {
  totalTrades: number;
  winningTrades: number;
  losingTrades: number;
  initialCapital: number;
  finalCapital: number;
  netPnl: number;
  totalFees: number;
  returnPct: number;
  maxDrawdown: number;
  maxDrawdownPct: number;
  sharpeRatio: number;
  sortinoRatio: number;
  calmarRatio: number;
  winRate: number;
  avgWin: number;
  avgLoss: number;
  profitFactor: number;
}

/** Stats attached to a `WalkForwardFold` and to a `GridSearchResult`.
 *  `RunnerStats` minus `avgWin` / `avgLoss`. */
export interface FoldStats {
  totalTrades: number;
  winningTrades: number;
  losingTrades: number;
  initialCapital: number;
  finalCapital: number;
  netPnl: number;
  totalFees: number;
  returnPct: number;
  maxDrawdown: number;
  maxDrawdownPct: number;
  sharpeRatio: number;
  sortinoRatio: number;
  calmarRatio: number;
  winRate: number;
  profitFactor: number;
}

// ── Symbol & registry ─────────────────────────────────────────────────

/** Result of `SymbolRegistry.addSymbol`. Coerces to its numeric `id`. */
export interface Symbol {
  readonly id: number;
  readonly name: string;
  readonly exchange: string;
  readonly tickSize: number;
  toString(): string;
  valueOf(): number;
}

export class SymbolRegistry {
  constructor();
  addSymbol(exchange: string, name: string, tickSize: number): Symbol;
  symbolCount(): number;
}

// ── Runner ────────────────────────────────────────────────────────────

/** One entry of `Runner.hookErrors()`. */
export interface HookErrorRecord {
  /** `"riskManager"` | `"killSwitch"` | `"orderValidator"` | `"executor"`. */
  hook: string;
  /** The method on the hook object that failed, e.g. `"allow"`. */
  method: string;
  /** The thrown message, or what was returned instead of a boolean. */
  message: string;
}

export class Runner {
  /** `threaded=true` runs callbacks on a background C++ Disruptor thread. */
  constructor(
    registry: SymbolRegistry,
    onSignal: (sig: Signal) => void,
    threaded?: boolean,
  );
  addStrategy(strategy: Strategy): void;
  /** Atomically swap the strategy at `index` for a new one. The old
   *  strategy's `onStop` fires before the swap; the new strategy's
   *  `onStart` fires after. Bus subscriptions, in-flight orders, and
   *  WebSocket / gRPC connections are untouched. Must be invoked on
   *  the V8 thread. */
  replaceStrategy(index: number, strategy: Strategy): void;
  start(): void;
  stop(): void;
  onTrade(
    symbol: Symbol | number,
    price: number,
    qty: number,
    isBuy: boolean,
    timestampNs: number | bigint,
  ): void;
  onBookSnapshot(
    symbol: Symbol | number,
    bidPrices: Float64Array,
    bidQtys: Float64Array,
    askPrices: Float64Array,
    askQtys: Float64Array,
    timestampNs: number | bigint,
  ): void;
  /** The nanosecond fields take a `Number` or a `BigInt`; a bar read out
   *  of `onBar` carries `BigInt`s and can be handed straight back. */
  onBar(
    symbol: Symbol | number,
    bar: Partial<Omit<BarData, "startTimeNs" | "endTimeNs">> &
      Pick<BarData, "open" | "high" | "low" | "close"> & {
        startTimeNs?: number | bigint;
        endTimeNs?: number | bigint;
      },
  ): void;

  // ── Hook setters ──
  //
  // Every one of these runs its JS on the thread the Runner dispatches
  // from, so all of them are sync only and all of them throw when the
  // Runner is `threaded`. Passing `null` detaches and is always allowed.
  /** Sync only — PnLTracker.onSignal is called inline. Throws if `threaded`. */
  setPnlTracker(tracker: PnLTracker | null): void;
  /** Sync only — StorageSink.store is called inline. Throws if `threaded`. */
  setStorageSink(sink: StorageSink | null): void;
  /** Sync only — RiskManager.allow is read inline. Throws if `threaded`. */
  setRiskManager(rm: RiskManager | null): void;
  /** Sync only — KillSwitch.check is read inline. Throws if `threaded`. */
  setKillSwitch(ks: KillSwitch | null): void;
  /** Sync only — OrderValidator.validate is read inline. Throws if `threaded`. */
  setOrderValidator(ov: OrderValidator | null): void;
  /** Sync only — the recorder's callbacks run inline. Throws if `threaded`.
   *  A `BinaryLogRecorderHook` records in C++ and calls no JS, but goes
   *  through the same setter and the same rule. */
  setMarketDataRecorder(recorder: MarketDataRecorderHook | BinaryLogRecorderHook | null): void;
  /** Sync only — Executor.capabilities() is read inline. Throws if `threaded`. */
  setExecutor(executor: Executor | null): void;

  /** Every hook failure this Runner has seen, oldest first.
   *
   *  A pre-trade gate (`RiskManager.allow`, `KillSwitch.check`,
   *  `OrderValidator.validate`) that throws, or hands back anything other
   *  than a boolean, denies the order and lands here; so does a throw out
   *  of `Executor.capabilities()`. A gate that returns `false` is a
   *  decision, not a failure, and is not recorded.
   *
   *  Readable synchronously after the call that failed. The process-wide
   *  log callback is asynchronous and shared, so it cannot say which
   *  Runner denied what. */
  hookErrors(): HookErrorRecord[];

  /** Auto-capture every signal into the given `.lrvxrun` recorder.
   *  Pass `null` to detach. Sync mode only; throws otherwise.
   *  Order / fill auto-capture is a follow-up — wire those through
   *  the executor's listener bus today. */
  attachTraceRecorder(recorder: TraceRecorder | null): void;
  /** Stamp every recorded signal with this `feed_ts_ns`. The runner
   *  copies it into each `SignalView.feed_ts_ns` slot until the next
   *  call. Useful for pinning replay determinism. */
  setTraceFeedTsNs(feedTsNs: number): void;

  /** Mirror an order event into the attached recorder. No-op when
   *  no recorder is attached. Wire from your executor wrapper after
   *  the corresponding `on_submitted` / `on_canceled` / etc. fires.
   *  `eventKind`: 1=Submit, 2=Cancel, 3=Modify, 4=Ack, 5=Reject,
   *               6=Expire. */
  traceOrderEvent(opts: {
    orderId: number;
    parentSignalId?: number;
    symbolId: number;
    eventKind: number;
    side: 0 | 1;
    orderType?: number;
    price?: number;
    qty?: number;
    flags?: number;
  }): void;

  /** Mirror a fill into the attached recorder. `liquidity`:
   *  0=Unknown, 1=Maker, 2=Taker. */
  traceFill(opts: {
    orderId: number;
    fillId?: number;
    price: number;
    qty: number;
    fee?: number;
    symbolId: number;
    side: 0 | 1;
    liquidity?: number;
  }): void;
}

// ── Backtest ──────────────────────────────────────────────────────────

export class Engine {
  /** Defaults: `initialCapital` 100000, `feeRate` 0.0001 (percentage). */
  constructor(initialCapital?: number, feeRate?: number);
  /** `symbol` defaults to the file stem, upper-cased with a trailing
   *  `_1m` / `_5m` / `_15m` / `_1h` / `_4h` / `_1d` stripped. */
  loadCsv(path: string, symbol?: string): void;
  /** All six arrays are required and must match `ts` in length.
   *  Timestamps auto-scale from s / ms / us to ns. `symbol` defaults
   *  to `"default"`. */
  loadOhlcv(
    bars: {
      ts: Float64Array;
      open: Float64Array;
      high: Float64Array;
      low: Float64Array;
      close: Float64Array;
      volume: Float64Array;
    },
    symbol?: string,
  ): void;
  /** Resample the bars of symbol `src` into symbol `dst`. `interval` is
   *  `<count><s|m|h|d>`, e.g. `"4h"`. Throws if `src` is not loaded. */
  resample(src: string, dst: string, interval: string): void;
  /** Replay a `SignalBuilder` against every loaded symbol. */
  run(signals: SignalBuilder): BacktestStats;
  /** `symbol` defaults to the first loaded symbol in every accessor
   *  below; an unknown name throws. */
  barCount(symbol?: string): number;
  /** Bar open times in nanoseconds. A `BigInt64Array`: the readings are
   *  exact int64 in the store, and a double steps 256 ns at present-day
   *  magnitudes. The price and volume accessors below stay `Float64Array`. */
  ts(symbol?: string): BigInt64Array;
  open(symbol?: string): Float64Array;
  high(symbol?: string): Float64Array;
  low(symbol?: string): Float64Array;
  close(symbol?: string): Float64Array;
  volume(symbol?: string): Float64Array;
  readonly symbols: string[];
}

export class SignalBuilder {
  constructor();
  /** `ts` auto-scales from s / ms / us to ns and takes a `Number` or a
   *  `BigInt`, so an `Engine.ts()` element goes straight in. `symbol`
   *  defaults to the engine's first loaded symbol at `run()` time.
   *  Returns `this`. */
  buy(ts: number | bigint, qty: number, symbol?: string): this;
  sell(ts: number | bigint, qty: number, symbol?: string): this;
  limitBuy(ts: number | bigint, price: number, qty: number, symbol?: string): this;
  limitSell(ts: number | bigint, price: number, qty: number, symbol?: string): this;
  clear(): void;
  readonly length: number;
}

/** Equity curve from the most recent run, returned by
 *  `BacktestRunner.equityCurve()`. Arrays are aligned: `equity[i]` and
 *  `drawdownPct[i]` correspond to `timestampNs[i]`. */
export interface EquityCurve {
  timestampNs: BigInt64Array;
  equity: Float64Array;
  drawdownPct: Float64Array;
}

/** Closed trades from the most recent run, returned by
 *  `BacktestRunner.trades()`. Arrays are aligned by row index.
 *  `side`: 0 = long (buy entry, sell exit), 1 = short. */
export interface BacktestTrades {
  symbol: Uint32Array;
  side: Uint8Array;
  entryPrice: Float64Array;
  exitPrice: Float64Array;
  quantity: Float64Array;
  pnl: Float64Array;
  fee: Float64Array;
  entryTimeNs: BigInt64Array;
  exitTimeNs: BigInt64Array;
}

/** Walk-forward fold metadata + train/test stats. */
export interface WalkForwardFold {
  foldIndex: number;
  trainStartBar: number;
  trainEndBar: number;
  testStartBar: number;
  testEndBar: number;
  trainStartNs: bigint;
  trainEndNs: bigint;
  testStartNs: bigint;
  testEndNs: bigint;
  trainStats: FoldStats;
  testStats: FoldStats;
}

/** Walk-forward configuration. `mode = 'anchored'`: train window
 *  expands from bar 0. `mode = 'sliding'`: fixed-size train window
 *  slides forward. */
export interface WalkForwardConfig {
  mode: 'anchored' | 'sliding';
  trainSize?: number;
  testSize: number;
  step?: number;
  minTrainSize?: number;
}

export class WalkForwardRunner {
  constructor(registry: SymbolRegistry, feeRate: number,
              initialCapital: number, config: WalkForwardConfig);
  /** Factory called twice per fold (train, then test) to build a
   *  fresh strategy object. */
  setStrategyFactory(factory: (foldIndex: number) => Strategy): void;
  runCsv(path: string, symbol: string): WalkForwardFold[];
}

/** Result row from `GridSearch.run()`. */
export interface GridSearchResult {
  index: number;
  params: number[];
  stats: FoldStats;
}

/** Type-erased grid search over numeric parameter axes. The last axis
 *  varies fastest. */
export class GridSearch {
  constructor();
  addAxis(values: number[]): void;
  /** The factory's return value is read key-by-key in the
   *  `BacktestRunner.run*` shape, so a `run*` result can be returned
   *  directly. Missing keys read as 0. */
  setFactory(factory: (params: number[]) => RunnerStats): void;
  total(): number;
  paramsForIndex(index: number): number[];
  run(): GridSearchResult[];
}

export class BacktestRunner {
  constructor(registry: SymbolRegistry, feeRate: number, initialCapital: number);
  setStrategy(strategy: Strategy): void;
  /** Returns `null` if `path` cannot be opened. */
  runCsv(path: string, symbol: string): RunnerStats | null;
  /** `timestamps` holds nanosecond epochs as a `BigInt64Array`. */
  runOhlcv(timestamps: BigInt64Array, closes: Float64Array, symbol: string): RunnerStats;
  /** Replay full OHLCV bars. `startNs` / `endNs` are `BigInt64Array`;
   *  the five OHLCV arrays are `Float64Array` and must match their
   *  length. `barType` uses the `BarData.barType` encoding (default 0 =
   *  time bar); `barTypeParam` is the interval / threshold (default 0). */
  runBars(
    startNs: BigInt64Array,
    endNs: BigInt64Array,
    opens: Float64Array,
    highs: Float64Array,
    lows: Float64Array,
    closes: Float64Array,
    volumes: Float64Array,
    symbol: string,
    barType?: number,
    barTypeParam?: number,
  ): RunnerStats;
  /** Replay a `.lrvx` tape directory through the backtest. The tape
   *  is the canonical recording format written by `lrvx tape record`
   *  (live) or `scripts/backfill_to_tape.py` (historical). Returns
   *  `null` if the tape cannot be read. */
  runTape(path: string): RunnerStats | null;
  /** Merge N `.lrvx` tape directories on read and replay the merged
   *  event stream through the backtest. Symbols are rekeyed by
   *  `(metadata.exchange, name)` so two venues stay distinct.
   *  `runTapes([t])` is equivalent to `runTape(t)`. Throws on bad paths
   *  or overlapping book streams across tapes. */
  runTapes(paths: string[]): RunnerStats;
  /** Replace the built-in SimulatedExecutor with a binding-supplied one. */
  setExecutor(executor: Executor | null): void;
  /** Attach a listener for order lifecycle events. Multiple listeners
   *  may be attached; each fires for every order event. */
  addExecutionListener(listener: ExecutionListener): void;
  /** Equity curve from the most recent run. Throws LrvxError(E_RUN_002)
   *  if no run has completed. */
  equityCurve(): EquityCurve;
  /** Closed trades from the most recent run. Throws LrvxError(E_RUN_002)
   *  if no run has completed. */
  trades(): BacktestTrades;
}

export class LatencyDistribution {
  constructor();
  setConstant(ns: number): void;
  setUniform(loNs: number, hiNs: number): void;
  setLognormal(medianNs: number, sigma: number): void;
  setEmpirical(samplesNs: number[]): void;
  setBurstCorrelation(rho: number): void;
  medianNs(): number;
}

export class FeeSchedule {
  constructor();
  addTier(minNotional30d: number, makerBps: number, takerBps: number): void;
  /** Canned: 'binance_um_futures' | 'bybit_linear' | 'okx_swap' | 'deribit'. */
  loadProfile(name: string): void;
  recordFill(tsNs: number, notional: number): void;
  feeFor(tsNs: number, notional: number, isMaker: boolean): number;
  currentTierIndex(): number;
  rollingNotional30d(): number;
  tierTransitions(): number[];
  resetRolling(): void;
  /** Bind an Account so 30d rolling notional reads from the account's
   *  cross-symbol aggregate counter instead of this schedule's
   *  internal one. */
  bindAccount(account: Account | null): void;
  clearAccountBinding(): void;
}

/** Cross-margin shared state owned by an account. Attach to a
 *  LiquidationEngine and/or FeeSchedule to share equity, positions,
 *  and 30d notional across symbols. */
export class Account {
  constructor(accountId?: number, equity?: number);
  accountId(): number;
  equity(): number;
  setEquity(equity: number): void;
  addEquity(delta: number): void;
  marginMode(): 'cross' | 'isolated';
  setMarginMode(mode: 'cross' | 'isolated' | number): void;
  openPosition(
    symbol: number,
    quantity: number,
    entryPrice: number,
    isolatedEquity?: number,
  ): void;
  closePosition(symbol: number): void;
  positionCount(): number;
  setMark(symbol: number, price: number, tsNs?: number): void;
  /** Last timestamp the symbol was marked. Very negative when never marked. */
  markTsFor(symbol: number): number;
  /** Returns true when any position's mark is older than budgetNs
   *  relative to nowNs, or has never been marked. */
  hasStaleMarks(nowNs: number, budgetNs: number): boolean;
  totalNotional(): number;
  totalUnrealisedPnl(): number;
  recordFill(tsNs: number, notional: number, symbol?: number): void;
  /** Per-symbol breakdown of rolling 30d notional. Returned as a list
   *  of `{ symbol, notional }` pairs. Symbol 0 is the "unknown / no
   *  symbol" bucket used by recordFill calls without a symbol arg. */
  rollingNotionalBySymbol30d(): Array<{ symbol: number; notional: number }>;
  rollingNotional30d(): number;
  resetRolling(): void;
}

/** Single-call venue-realistic backtest stack.
 *
 *  Use one of the static factories to obtain a fully-wired stack
 *  (executor + account + liquidation + fees + funding + rate limits +
 *  venue availability). For backtests of real strategies always go
 *  through a venue factory — the bare SimulatedExecutor() constructor
 *  is for unit tests of the executor itself.
 *
 *  TypeScript exposes a flat proxy surface (accountOpenPosition,
 *  liquidationOnMark, feesRecordFill, etc.) instead of returning
 *  non-owning subsystem wraps. */
export class VenueStack {
  /** Construct via static factory; the raw constructor takes
   *  (venueCode, accountId, equity) where venueCode is
   *  0=binance_um_futures, 1=bybit_linear, 2=okx_swap, 3=deribit. */
  constructor(venueCode: number, accountId: number, equity: number);
  static binanceUmFutures(accountId: number, equity: number): VenueStack;
  static bybitLinear(accountId: number, equity: number): VenueStack;
  static okxSwap(accountId: number, equity: number): VenueStack;
  static deribit(accountId: number, equity: number): VenueStack;
  static fromVenue(
    name: 'binance_um_futures' | 'bybit_linear' | 'okx_swap' | 'deribit' | string,
    accountId: number,
    equity: number,
  ): VenueStack;
  venueName(): string;
  // Account proxies.
  accountId(): number;
  accountEquity(): number;
  accountSetEquity(equity: number): void;
  accountAddEquity(delta: number): void;
  accountOpenPosition(symbol: number, quantity: number, entryPrice: number): void;
  accountClosePosition(symbol: number): void;
  accountPositionCount(): number;
  accountSetMark(symbol: number, price: number): void;
  accountTotalNotional(): number;
  accountTotalUnrealisedPnl(): number;
  accountRollingNotional30d(): number;
  // Liquidation proxies.
  liquidationOnMark(symbol: number, markPrice: number): number;
  liquidationsCount(): number;
  insuranceFundBalance(): number;
  // Fees proxies.
  feesRecordFill(tsNs: number, notional: number): void;
  feesCurrentTierIndex(): number;
  feesFor(tsNs: number, notional: number, isMaker: boolean): number;
}

export interface FundingPaymentEvent {
  timestampNs: number;
  symbol: number;
  rate: number;
  markPrice: number;
  positionSigned: number;
  amount: number;
}

/** Liquidation engine + insurance fund + auto-deleverage (ADL).
 *  Holds maintenance-margin tiers, an insurance fund balance, and
 *  the open-position book. `onMark(symbol, mark)` walks every
 *  position, fires liquidations / ADL where required, and updates
 *  cumulative stats. Returns the number of accounts liquidated on
 *  this tick. */
export class LiquidationEngine {
  constructor();
  addTier(minNotional: number, mmFraction: number): void;
  setInsuranceFundCapital(capital: number): void;
  insuranceFundBalance(): number;
  setAdlEnabled(enabled: boolean): void;
  /** ADL queue ordering. Accepts a string name or a numeric code:
   *  'pnl_ratio' | 'binance' | 'bybit' | 'position_size' (0..3). */
  setAdlRanking(ranking: 'pnl_ratio' | 'binance' | 'bybit' | 'position_size' | number): void;
  adlRanking(): 'pnl_ratio' | 'binance' | 'bybit' | 'position_size';
  setLiquidationSlippageBps(bps: number): void;
  /** Open a position the engine should watch. Side is encoded in
   *  signed `quantity` (positive = long, negative = short). */
  openPosition(
    accountId: number,
    symbol: number,
    quantity: number,
    entryPrice: number,
    equity: number,
  ): void;
  closePosition(accountId: number, symbol: number): void;
  onMark(symbol: number, markPrice: number): number;
  /** Multi-symbol atomic mark update + liquidation walk. Pass tuples
   *  `[symbol, price]` and an optional ts_ns for the stale-mark guard.
   *  Returns aggregated liquidations count across all walked symbols. */
  onMarks(marks: Array<[number, number]>, tsNs?: number): number;
  liquidationsCount(): number;
  insurancePaymentsCount(): number;
  adlCloseoutsCount(): number;
  /** Canned profiles: 'binance_um_futures' | 'bybit_linear' | 'okx_swap'. */
  loadProfile(name: string): void;
  /** Attach a SimulatedExecutor so liquidation orders route through
   *  it as market orders (consuming book liquidity, paying fees,
   *  sampling latency). Pass null to detach. */
  setExecutor(executor: SimulatedExecutor | null): void;
  /** Per-payment USD covered by the insurance fund. */
  deficitsPaidByFund(): number[];
  /** Per-closeout USD absorbed by an ADL force-close. */
  deficitsPaidByAdl(): number[];
  /** Liquidations per onMark tick that liquidated anything. */
  cascadeSizesPerTick(): number[];
  /** Insurance fund balance after each tick that touched it. */
  fundBalanceHistory(): number[];
  /** Ordinal of the first ADL tick; very large value if no ADL yet. */
  ticksToFirstAdl(): number;
  resetStats(): void;
  /**
   * Configure mark-impact feedback after liquidation fills. When the
   * model is not 'none' and an executor is attached, the engine
   * recomputes the mark from the post-cascade book mid and may
   * trigger additional liquidations within the same onMark call.
   *
   *   none           — mark stays at the tape input (default).
   *   book_anchored  — mark = (1 - weight) * tape + weight * book_mid.
   *   book_only      — mark = book_mid (falls back to tape if no book).
   */
  setMarkImpactModel(
    model: 'none' | 'book_anchored' | 'book_only' | number,
    weight?: number,
  ): void;
  markImpactModel(): 'none' | 'book_anchored' | 'book_only';
  markImpactWeight(): number;
  /** Bound on second-order cascade depth in a single onMark call. 0 disables. */
  setMaxCascadeDepth(depth: number): void;
  maxCascadeDepth(): number;
  /** Attach an Account so its positions participate in cross-margin
   *  MM checks. Multiple accounts may be attached. */
  attachAccount(account: Account): void;
  detachAccount(accountId: number): void;
}

export class FundingSchedule {
  constructor();
  setConstant(intervalNs: number, rate: number): void;
  setTape(timestampsNs: number[], rates: number[]): void;
  /** Per-symbol tape: each row settles only the matching symbol at that
   *  timestamp. symbols[i] == 0 acts as a wildcard. Symbols without an
   *  entry at a given settlement timestamp fall back to the constant
   *  rate (defaults to 0). */
  setTapeBySymbol(
    timestampsNs: number[],
    symbols: number[],
    rates: number[],
  ): void;
  /** Load a CSV with columns timestamp_ns,symbol,funding_rate. Lines
   *  starting with '#' and a leading alphabetic header row are skipped.
   *  Returns true on success, false if the file couldn't be opened. */
  loadTape(path: string): boolean;
  /** Canned: 'binance_um_futures' | 'bybit_linear' | 'okx_swap' | 'bitget_hourly'. */
  loadProfile(name: string): void;
  setConstantRate(rate: number): void;
  reset(): void;
  tick(
    nowNs: number,
    symbols: number[],
    positions: number[],
    markPrices: number[],
  ): FundingPaymentEvent[];
}

export interface RateLimitBucketState {
  windowNs: number;
  used: number;
  capacity: number;
}

/** Outage policy for orders open when a venue outage begins. */
export type OnOutage = 'cancel_all' | 'hold' | 'expire_gtc_after';

/** Models venue downtime — scheduled maintenance windows and random
 *  disconnects. Attach to a `SimulatedExecutor` via
 *  `setVenueAvailability` to make submit / cancel / replace buffer
 *  during outages and market-data callbacks be dropped (feed gap).
 *  Buffered requests flush at the recovery edge in FIFO order. */
export class VenueAvailability {
  constructor();
  /** Schedule a one-shot outage at `startNs` lasting `durationNs`.
   *  `gtcTtlNs` is honored only when `onOpenOrders` is
   *  `'expire_gtc_after'`. */
  scheduleOutage(
    startNs: number,
    durationNs: number,
    onOpenOrders?: OnOutage,
    gtcTtlNs?: number,
  ): void;
  /** Enable Poisson random outages. `perDay` is the expected number
   *  of outages per UTC day; `meanDurationNs` is the mean of the
   *  exponential outage duration. `seed` makes sampling
   *  reproducible. */
  autoRandomOutages(
    perDay: number,
    meanDurationNs: number,
    onOpenOrders?: OnOutage,
    seed?: number,
  ): void;
  /** Extended schedule with outage pathology. outageType:
   *  'total' (default) | 'submit_only_down' | 'cancel_only_down' |
   *  'slow_degradation' | 'stale_book' | 'wrong_side_recovery'. */
  scheduleOutageEx(
    startNs: number,
    durationNs: number,
    outageType?:
      | 'total'
      | 'submit_only_down'
      | 'cancel_only_down'
      | 'slow_degradation'
      | 'stale_book'
      | 'wrong_side_recovery',
    onOpenOrders?: OnOutage,
    gtcTtlNs?: number,
    degradationLatencyMultiplier?: number,
    wrongSideRecoveryBps?: number,
  ): void;
  isUp(nowNs: number): boolean;
  submitsAllowed(nowNs: number): boolean;
  cancelsAllowed(nowNs: number): boolean;
  bookUpdatesAllowed(nowNs: number): boolean;
  tradesAllowed(nowNs: number): boolean;
  /** Latency multiplier applied to submit/cancel/replace ack times
   *  while a SlowDegradation outage is active; 1.0 otherwise. */
  latencyMultiplier(nowNs: number): number;
  /** Pop the wrong-side-recovery bps accumulated when a
   *  WrongSideRecovery outage just ended. Idempotent — subsequent
   *  calls return 0 until the next such outage ends. */
  consumeWrongSideRecoveryBps(): number;
}

export class RateLimitPolicy {
  constructor();
  addBucket(
    name: string,
    windowNs: number,
    capacity: number,
    submitWeight?: number,
    cancelWeight?: number,
    replaceWeight?: number,
  ): void;
  /** Add a bucket scoped to a specific endpoint family. The
   *  Trading-family weights are not consumed by Account /
   *  MarketData traffic, so a strategy that pounds position queries
   *  can't exhaust its trading quota. */
  addFamilyBucket(
    family: 'trading' | 'market_data' | 'account' | number,
    name: string,
    windowNs: number,
    capacity: number,
    queryWeight?: number,
  ): void;
  setBan(afterConsecutiveRejects: number, banDurationNs: number): void;
  /** Canned profile: 'binance_um_futures' | 'bybit_linear' | 'okx_swap' | 'deribit'. */
  loadProfile(name: string): void;
  banUntilNs(): number;
  consecutiveRejects(): number;
  bucketStates(nowNs: number): RateLimitBucketState[];
}

export class SimulatedExecutor {
  constructor();
  submitOrder(
    id: number,
    side: Side,
    price: number,
    qty: number,
    type: OrderType,
    symbol: number,
    opts?: {
      tif?: 'gtc' | 'ioc' | 'fok' | 'gtd' | 'post_only';
      reduceOnly?: boolean;
      expiresAtNs?: number;
    },
  ): void;
  cancelOrder(orderId: number): void;
  cancelAll(symbol: number): void;
  onBar(symbol: number, closePrice: number): void;
  /** Open-aware form: moves the market to the open (releasing any order held
   *  from the previous bar's callback there), then walks low -> high -> close
   *  so resting stops/targets match the intrabar extremes. Use this, not
   *  onBar, when driving the executor by hand to reach the fills
   *  BacktestRunner.runBars produces on the same bars. */
  onBarOhlc(symbol: number, open: number, high: number, low: number, close: number): void;
  /** Open the bar-callback window: every order submitted while it is open is
   *  held instead of matched immediately, and released at the next bar's
   *  open. Call before invoking a strategy's bar callback by hand; always
   *  pair with endBarCallbackWindow. */
  beginBarCallbackWindow(): void;
  /** Close the bar-callback window opened by beginBarCallbackWindow. */
  endBarCallbackWindow(): void;
  /** Drops fills and run-scoped state while keeping installed configuration
   *  (slippage, queue model, latency, ...), so a second hand-driven run
   *  reports that run and not the sum of every run. */
  reset(): void;
  onTrade(symbol: number, price: number, isBuy: boolean): void;
  advanceClock(timestampNs: number | bigint): void;
  setDefaultSlippage(
    model: SlippageModel | number,
    ticks: number,
    tickSize: number,
    bps: number,
    impactCoeff: number,
  ): void;
  /** `depth` is the number of book levels the model tracks; defaults to 1. */
  setQueueModel(model: QueueModel | number, depth?: number): void;
  /** For 'pro_rata_with_fifo': the first N orders at a price level
   *  consume the trade FIFO; the remainder is split pro-rata across
   *  the rest. Ignored in 'none' / 'tob' / 'full' / 'pro_rata'. */
  setQueueFifoTopN(topN: number): void;
  /** TOP_PRO_LMM: fraction of each trade reserved for the queue-front
   *  order (capped by its remaining). Default 0.40. */
  setTopPriorityShare(share: number): void;
  /** TOP_PRO_LMM: mark these order ids as Lead Market Makers. LMM
   *  orders get an implicit bonus multiplier during tail pro-rata. */
  setLmmOrders(ids: number[]): void;
  /** TOP_PRO_LMM: LMM bonus multiplier on tail pro-rata distribution. Default 1.5. */
  setLmmBonusMultiplier(multiplier: number): void;
  /** TOP_PRO_LMM / PRO_RATA_WITH_PRIORITY: per-order priority weight
   *  (default 1.0). Effective allocation weight = remaining × multiplier. */
  setOrderPriorityMultiplier(orderId: number, multiplier: number): void;
  /** Configure submit ack latency for backtest realism. Zero disables the
   *  async submit path. */
  setSubmitAckLatency(latencyNs: number, jitterNs?: number): void;
  /** Configure cancel ack latency. */
  setCancelAckLatency(latencyNs: number, jitterNs?: number): void;
  /** Configure replace ack latency. */
  setReplaceAckLatency(latencyNs: number, jitterNs?: number): void;
  /** Distribution-based ack latency setters. Distribution is copied. */
  setSubmitAckLatencyDistribution(dist: LatencyDistribution): void;
  setCancelAckLatencyDistribution(dist: LatencyDistribution): void;
  setReplaceAckLatencyDistribution(dist: LatencyDistribution): void;
  /** Apply a named latency profile. Names: "binance_um_futures",
   *  "bybit_linear", "okx_swap", "deribit", "idealized", "adversarial". */
  applyLatencyProfile(name: string): void;
  /** Attach a client-side rate-limit policy. Submit / cancel / replace
   *  consult the policy first; an overflow emits a rate-limit reject. */
  setRateLimitPolicy(policy: RateLimitPolicy): void;
  clearRateLimitPolicy(): void;
  /** Self-trade prevention mode. 'none' | 'cancel_newest' | 'cancel_oldest'
   *  | 'cancel_both' | 'decrement'. Default 'none'. */
  setSTPMode(
    mode: 'none' | 'cancel_newest' | 'cancel_oldest' | 'cancel_both' | 'decrement',
  ): void;
  /** Opt an STP account into a group. groupId=0 removes the mapping.
   *  Two orders share an STP scope when their accountIds match OR
   *  both accounts map into the same non-zero group. */
  setSTPGroupMembership(accountId: number, groupId: number): void;
  stpGroupFor(accountId: number): number;
  /** FOK fill semantic. 'any_price' (default) walks crossing book qty;
   *  'single_price' requires the level at the limit price to hold the
   *  full order qty. */
  setFokMode(mode: 'any_price' | 'single_price'): void;
  fokMode(): 'any_price' | 'single_price';
  /** Attach a VenueAvailability model. Pass null to detach. */
  setVenueAvailability(availability: VenueAvailability | null): void;
  /** Submit a native bracket order (entry + take-profit + stop). */
  submitBracket(opts: {
    bracketId: number;
    symbol?: number;
    entrySide: Side;
    entryType: 'limit' | 'market' | 'stop_market' | 'stop_limit';
    entryPrice: number;
    quantity: number;
    tpSide: Side;
    tpType: 'limit' | 'market' | 'stop_market' | 'stop_limit';
    tpPrice: number;
    stopSide: Side;
    stopType: 'limit' | 'market' | 'stop_market' | 'stop_limit';
    stopTriggerPrice: number;
  }): void;
  cancelBracket(bracketId: number): void;
  bracketState(
    bracketId: number,
  ): 'pending_entry' | 'entry_filled' | 'tp_filled' | 'stop_filled' | 'canceled';
  /** Bracket child-arm policy: 'on_full_fill' (default) arms TP+stop
   *  once on full entry fill; 'on_partial_fill' arms / resizes
   *  children incrementally on every partial entry fill. */
  setBracketChildArmMode(mode: 'on_full_fill' | 'on_partial_fill'): void;
  /** Submit a native iceberg order. Only `visibleQuantity` is exposed
   *  to the book; the hidden remainder (total - visible) refreshes
   *  automatically as the visible tranche fills. */
  submitIceberg(
    orderId: number,
    side: Side,
    price: number,
    totalQuantity: number,
    visibleQuantity: number,
    symbol?: number,
  ): void;
  /** Default refresh latency (ns) between a visible tranche filling
   *  and the next one being exposed to the book. Zero = instant. */
  setIcebergRefreshLatency(latencyNs: number): void;
  /** Diagnostic: remaining hidden quantity (raw fixed-point) for an
   *  iceberg order, or 0 if none. */
  icebergHiddenRemainingRaw(orderId: number): number;
  /** Per-refresh visible-slice size jitter as a fraction.
   *  0.0 = deterministic (default); 0.10 = ±10% uniform. */
  setIcebergSizeRandomisationPct(pct: number): void;
  /** Queue priority on refresh. 'back' (default) sends the refreshed
   *  slice to the back of the queue; 'retain' keeps it in the prior
   *  slice's position (CME-style). */
  setIcebergPriorityMode(mode: 'back' | 'retain'): void;
  /** Seed the size-jitter RNG for reproducible refresh sequences. */
  setIcebergJitterSeed(seed: number): void;
  readonly fillCount: number;
}

export class BacktestResult {
  constructor(initialCapital: number, feeRate: number);
  recordFill(
    orderId: number,
    symbol: number,
    side: Side,
    price: number,
    qty: number,
    timestampNs: number | bigint,
  ): void;
  ingestExecutor(executor: SimulatedExecutor): void;
  stats(): BacktestStats;
}

// ── Streaming indicators ──────────────────────────────────────────────

interface StreamingSingleInput {
  /** Returns the current value (or null until warmed up). */
  update(value: number): number | null;
  /** Current value (NaN until warmed up). */
  readonly value: number;
  /** True once `update` has been called enough times to produce a value. */
  readonly ready: boolean;
  /** Reset state but keep configuration. */
  reset(): void;
  /** Batch convenience: apply `update` over an array, returning the values. */
  compute(input: Float64Array): Float64Array;
}

export class SMA implements StreamingSingleInput {
  constructor(period: number);
  update(value: number): number | null;
  readonly value: number;
  readonly ready: boolean;
  reset(): void;
  compute(input: Float64Array): Float64Array;
}
export class EMA implements StreamingSingleInput {
  constructor(period: number);
  update(value: number): number | null;
  readonly value: number;
  readonly ready: boolean;
  reset(): void;
  compute(input: Float64Array): Float64Array;
}
export class RMA implements StreamingSingleInput {
  constructor(period: number);
  update(value: number): number | null;
  readonly value: number;
  readonly ready: boolean;
  reset(): void;
  compute(input: Float64Array): Float64Array;
}
export class RSI implements StreamingSingleInput {
  constructor(period: number);
  update(value: number): number | null;
  readonly value: number;
  readonly ready: boolean;
  reset(): void;
  compute(input: Float64Array): Float64Array;
}
export class DEMA implements StreamingSingleInput {
  constructor(period: number);
  update(value: number): number | null;
  readonly value: number;
  readonly ready: boolean;
  reset(): void;
  compute(input: Float64Array): Float64Array;
}
export class TEMA implements StreamingSingleInput {
  constructor(period: number);
  update(value: number): number | null;
  readonly value: number;
  readonly ready: boolean;
  reset(): void;
  compute(input: Float64Array): Float64Array;
}
export class KAMA implements StreamingSingleInput {
  constructor(period: number, fast?: number, slow?: number);
  update(value: number): number | null;
  readonly value: number;
  readonly ready: boolean;
  reset(): void;
  compute(input: Float64Array): Float64Array;
}
export class Slope implements StreamingSingleInput {
  constructor(length: number);
  update(value: number): number | null;
  readonly value: number;
  readonly ready: boolean;
  reset(): void;
  compute(input: Float64Array): Float64Array;
}
export class Skewness implements StreamingSingleInput {
  constructor(period: number);
  update(value: number): number | null;
  readonly value: number;
  readonly ready: boolean;
  reset(): void;
  compute(input: Float64Array): Float64Array;
}
export class Kurtosis implements StreamingSingleInput {
  constructor(period: number);
  update(value: number): number | null;
  readonly value: number;
  readonly ready: boolean;
  reset(): void;
  compute(input: Float64Array): Float64Array;
}
export class RollingZScore implements StreamingSingleInput {
  constructor(period: number);
  update(value: number): number | null;
  readonly value: number;
  readonly ready: boolean;
  reset(): void;
  compute(input: Float64Array): Float64Array;
}
export class ShannonEntropy {
  constructor(period: number, bins: number);
  update(value: number): number | null;
  readonly value: number;
  readonly ready: boolean;
  reset(): void;
  compute(input: Float64Array): Float64Array;
}
export class AutoCorrelation {
  constructor(window: number, lag: number);
  update(value: number): number | null;
  readonly value: number;
  readonly ready: boolean;
  reset(): void;
  compute(input: Float64Array): Float64Array;
}

/** Streaming OHLC indicator: `update(high, low, close)`. */
export class ATR {
  constructor(period: number);
  update(high: number, low: number, close: number): number | null;
  readonly value: number;
  readonly ready: boolean;
  reset(): void;
  compute(high: Float64Array, low: Float64Array, close: Float64Array): Float64Array;
}
export class CCI {
  constructor(period: number);
  update(high: number, low: number, close: number): number | null;
  readonly value: number;
  readonly ready: boolean;
  reset(): void;
  compute(high: Float64Array, low: Float64Array, close: Float64Array): Float64Array;
}
export class Stochastic {
  constructor(kPeriod: number, dPeriod?: number);
  update(high: number, low: number, close: number): { k: number | null; d: number | null };
  readonly k: number;
  readonly d: number;
  readonly ready: boolean;
  reset(): void;
  compute(
    high: Float64Array,
    low: Float64Array,
    close: Float64Array,
  ): { k: Float64Array; d: Float64Array };
}
export class ParkinsonVol {
  constructor(period: number);
  update(high: number, low: number): number | null;
  readonly value: number;
  readonly ready: boolean;
  reset(): void;
  compute(high: Float64Array, low: Float64Array): Float64Array;
}
export class RogersSatchellVol {
  constructor(period: number);
  update(open: number, high: number, low: number, close: number): number | null;
  readonly value: number;
  readonly ready: boolean;
  reset(): void;
  compute(
    open: Float64Array,
    high: Float64Array,
    low: Float64Array,
    close: Float64Array,
  ): Float64Array;
}
export class Correlation {
  constructor(period: number);
  update(x: number, y: number): number | null;
  readonly value: number;
  readonly ready: boolean;
  reset(): void;
  compute(x: Float64Array, y: Float64Array): Float64Array;
}

/** Multi-output: MACD line/signal/histogram. */
export class MACD {
  constructor(fast?: number, slow?: number, signal?: number);
  update(value: number): { line: number | null; signal: number | null; histogram: number | null };
  readonly line: number;
  readonly signal: number;
  readonly histogram: number;
  readonly ready: boolean;
  reset(): void;
  compute(input: Float64Array): { line: Float64Array; signal: Float64Array; histogram: Float64Array };
}
/** Multi-output: Bollinger upper/middle/lower bands. */
export class Bollinger {
  constructor(period: number, stdDev?: number);
  update(value: number): { upper: number | null; middle: number | null; lower: number | null };
  readonly upper: number;
  readonly middle: number;
  readonly lower: number;
  readonly ready: boolean;
  reset(): void;
  compute(input: Float64Array): { upper: Float64Array; middle: Float64Array; lower: Float64Array };
}

// ── Batch indicator functions ─────────────────────────────────────────

export function sma(input: Float64Array, period: number): Float64Array;
export function ema(input: Float64Array, period: number): Float64Array;
export function rma(input: Float64Array, period: number): Float64Array;
export function rsi(input: Float64Array, period: number): Float64Array;
export function dema(input: Float64Array, period: number): Float64Array;
export function tema(input: Float64Array, period: number): Float64Array;
export function kama(input: Float64Array, period: number, fast?: number, slow?: number): Float64Array;
export function slope(input: Float64Array, length: number): Float64Array;
export function skewness(input: Float64Array, period: number): Float64Array;
export function kurtosis(input: Float64Array, period: number): Float64Array;
export function rolling_zscore(input: Float64Array, period: number): Float64Array;
export function shannon_entropy(input: Float64Array, period: number, bins: number): Float64Array;
export function autocorrelation(input: Float64Array, window: number, lag: number): Float64Array;
/** Rolling Pearson correlation over a moving `period` window. */
export function rollingCorrelation(x: Float64Array, y: Float64Array, period: number): Float64Array;
/** Augmented Dickey-Fuller unit-root test. `regression` is the
 *  deterministic term: "c" (constant, default), "ct" (constant + trend),
 *  or "n" (none). */
export function adf(
  input: Float64Array,
  maxLag?: number,
  regression?: "c" | "ct" | "n",
): { testStat: number; pValue: number; usedLag: number };
export function chop(high: Float64Array, low: Float64Array, close: Float64Array, period: number): Float64Array;
export function atr(high: Float64Array, low: Float64Array, close: Float64Array, period: number): Float64Array;
export function cci(high: Float64Array, low: Float64Array, close: Float64Array, period: number): Float64Array;
export function parkinson_vol(high: Float64Array, low: Float64Array, period: number): Float64Array;
export function rogers_satchell_vol(
  open: Float64Array,
  high: Float64Array,
  low: Float64Array,
  close: Float64Array,
  period: number,
): Float64Array;
export function adx(
  high: Float64Array,
  low: Float64Array,
  close: Float64Array,
  period: number,
): { adx: Float64Array; plusDi: Float64Array; minusDi: Float64Array };
export function bollinger(
  input: Float64Array,
  period: number,
  stdDev: number,
): { upper: Float64Array; middle: Float64Array; lower: Float64Array };
export function macd(
  input: Float64Array,
  fast: number,
  slow: number,
  signal: number,
): { line: Float64Array; signal: Float64Array; histogram: Float64Array };
export function stochastic(
  high: Float64Array,
  low: Float64Array,
  close: Float64Array,
  kPeriod: number,
  dPeriod: number,
): { k: Float64Array; d: Float64Array };
export function obv(close: Float64Array, volume: Float64Array): Float64Array;
export function vwap(close: Float64Array, volume: Float64Array, window: number): Float64Array;
export function cvd(
  open: Float64Array,
  high: Float64Array,
  low: Float64Array,
  close: Float64Array,
  volume: Float64Array,
): Float64Array;
/** Returns the list of indicator names (used by the catalog generator). */
export function list_indicators(): string[];

// ── Bar aggregation ───────────────────────────────────────────────────

/** A single aggregated bar emitted by `aggregate*` helpers. */
export interface AggregatedBar {
  startTimeNs: bigint;
  endTimeNs: bigint;
  open: number;
  high: number;
  low: number;
  close: number;
  volume: number;
  buyVolume: number;
  tradeCount: number;
}

export function aggregateTimeBars(
  timestamps: Float64Array | BigInt64Array,
  prices: Float64Array,
  quantities: Float64Array,
  isBuy: Uint8Array,
  intervalSeconds: number,
): AggregatedBar[];
export function aggregateTickBars(
  timestamps: Float64Array | BigInt64Array,
  prices: Float64Array,
  quantities: Float64Array,
  isBuy: Uint8Array,
  tickCount: number,
): AggregatedBar[];
export function aggregateVolumeBars(
  timestamps: Float64Array | BigInt64Array,
  prices: Float64Array,
  quantities: Float64Array,
  isBuy: Uint8Array,
  threshold: number,
): AggregatedBar[];
export function aggregateRangeBars(
  timestamps: Float64Array | BigInt64Array,
  prices: Float64Array,
  quantities: Float64Array,
  isBuy: Uint8Array,
  rangeSize: number,
): AggregatedBar[];
export function aggregateRenkoBars(
  timestamps: Float64Array | BigInt64Array,
  prices: Float64Array,
  quantities: Float64Array,
  isBuy: Uint8Array,
  brickSize: number,
): AggregatedBar[];
export function aggregateHeikinAshiBars(
  timestamps: Float64Array | BigInt64Array,
  prices: Float64Array,
  quantities: Float64Array,
  isBuy: Uint8Array,
  intervalSeconds: number,
): AggregatedBar[];

// ── Order books ───────────────────────────────────────────────────────

export class OrderBook {
  constructor(tickSize: number);
  applySnapshot(
    bidPrices: Float64Array,
    bidQtys: Float64Array,
    askPrices: Float64Array,
    askQtys: Float64Array,
  ): void;
  applyDelta(
    bidPrices: Float64Array,
    bidQtys: Float64Array,
    askPrices: Float64Array,
    askQtys: Float64Array,
  ): void;
  bestBid(): number | null;
  bestAsk(): number | null;
  mid(): number | null;
  spread(): number | null;
  /** Up to `n` levels; silently clamped to 100 (the Python binding is uncapped). */
  getBids(n: number): Array<[number, number]>;
  /** Up to `n` levels; silently clamped to 100 (the Python binding is uncapped). */
  getAsks(n: number): Array<[number, number]>;
  isCrossed(): boolean;
  clear(): void;
}

export class L3Book {
  constructor();
  /** Returns 0 on success. */
  addOrder(orderId: number, price: number, qty: number, side: Side): number;
  removeOrder(orderId: number): number;
  modifyOrder(orderId: number, newQty: number): number;
  bestBid(): number | null;
  bestAsk(): number | null;
  bidAtPrice(price: number): number;
  askAtPrice(price: number): number;
}

export class CompositeBookMatrix {
  constructor();
  applySnapshot(
    exchange: number,
    symbol: number,
    bidPrices: Float64Array,
    bidQtys: Float64Array,
    askPrices: Float64Array,
    askQtys: Float64Array,
    recvNs?: number | bigint,
  ): void;
  applyDelta(
    exchange: number,
    symbol: number,
    bidPrices: Float64Array,
    bidQtys: Float64Array,
    askPrices: Float64Array,
    askQtys: Float64Array,
    recvNs?: number | bigint,
  ): void;
  bestBid(symbol: number): { price: number; qty: number } | null;
  bestAsk(symbol: number): { price: number; qty: number } | null;
  hasArbitrage(symbol: number): boolean;
  markStale(exchange: number, symbol: number): void;
  checkStaleness(nowNs: number | bigint, thresholdNs: number | bigint): void;
}

// ── Position / order tracking ─────────────────────────────────────────

export class PositionTracker {
  constructor(mode?: PositionMode);
  onFill(symbol: number, side: Side, price: number, qty: number): void;
  position(symbol: number): number;
  avgEntryPrice(symbol: number): number;
  realizedPnl(symbol: number): number;
  totalRealizedPnl(): number;
}

export class PositionGroupTracker {
  constructor();
  /** Returns position ID. */
  openPosition(orderId: number, symbol: number, side: Side, price: number, qty: number): number;
  closePosition(positionId: number, exitPrice: number): void;
  partialClose(positionId: number, qty: number, exitPrice: number): void;
  netPosition(symbol: number): number;
  realizedPnl(symbol: number): number;
  totalRealizedPnl(): number;
  openCount(symbol: number): number;
  prune(): void;
}

export class OrderTracker {
  constructor();
  onSubmitted(orderId: number, symbol: number, side: Side, price: number, qty: number): boolean;
  onFilled(orderId: number, fillQty: number): boolean;
  onCanceled(orderId: number): boolean;
  isActive(orderId: number): boolean;
  readonly activeCount: number;
  readonly totalCount: number;
  prune(): void;
}

/** One row of an order-journey trace. */
export interface OrderTraceRow {
  orderId: number;
  seq: number;
  status: number;
  isMaker: boolean;
  tsNs: number;
  fillQty: number;
  fillPrice: number;
  queueAhead: number;
  queueTotal: number;
  submittedAtNs: number;
  acceptedAtNs: number;
  firstFillAtNs: number;
  lastFillAtNs: number;
  canceledAtNs: number;
  rejectedAtNs: number;
  triggeredAtNs: number;
  expiredAtNs: number;
}

/** Execution listener that records the full event sequence of every
 *  order it observes. Attach to a BacktestRunner via
 *  addJourneyTracer(); produces structured traces for post-trade
 *  forensics. */
export class OrderJourneyTracer {
  constructor(opts?: {
    maxOrders?: number;
    maxRecordsPerOrder?: number;
    sampleRate?: number;
    sampleSalt?: number;
  });
  orderCount(): number;
  recordCount(): number;
  medianAckLatencyNs(): number;
  medianTimeToFirstFillNs(): number;
  makerFillRatio(): number;
  cancelRaceLossRate(): number;
  result(): OrderTraceRow[];
  journey(orderId: number): OrderTraceRow[];
  clear(): void;
}

// ── Profiles ──────────────────────────────────────────────────────────

export class VolumeProfile {
  constructor(tickSize: number);
  addTrade(price: number, qty: number, isBuy: boolean): void;
  poc(): number;
  valueAreaHigh(): number;
  valueAreaLow(): number;
  totalVolume(): number;
  clear(): void;
}

export class MarketProfile {
  constructor(tickSize: number, periodMinutes: number, sessionStartNs: number | bigint);
  addTrade(timestampNs: number | bigint, price: number, qty: number, isBuy: boolean): void;
  poc(): number;
  valueAreaHigh(): number;
  valueAreaLow(): number;
  initialBalanceHigh(): number;
  initialBalanceLow(): number;
  isPoorHigh(): boolean;
  isPoorLow(): boolean;
  clear(): void;
}

export class FootprintBar {
  constructor(tickSize: number);
  addTrade(price: number, qty: number, isBuy: boolean): void;
  totalDelta(): number;
  totalVolume(): number;
  readonly numLevels: number;
  clear(): void;
}

// ── Statistics ────────────────────────────────────────────────────────

/**
 * Pearson correlation coefficient over the entire pair of arrays.
 * For a rolling correlation see {@link rollingCorrelation}.
 */
export function correlation(x: Float64Array, y: Float64Array): number;
export function profitFactor(pnl: Float64Array): number;
export function winRate(pnl: Float64Array): number;
export function bootstrapCI(
  data: Float64Array,
  confidence?: number,
  samples?: number,
): { lower: number; median: number; upper: number };
export function permutationTest(
  group1: Float64Array,
  group2: Float64Array,
  samples?: number,
): number;
/**
 * White's reality check (Stationary Bootstrap).
 *
 * Tests whether the best-performing strategy among `numStrategies`
 * candidates is significantly better than zero, after correcting for
 * the multiple-comparison bias from picking the best.
 *
 * `returns` is a flat row-major matrix of EXCESS returns (each
 * strategy's series concatenated): length must be at least
 * `numStrategies * numPeriods`. The caller is responsible for
 * subtracting any benchmark.
 *
 * `avgBlockSize` controls the stationary-bootstrap mean block length;
 * `0` (default) uses `sqrt(numPeriods)`.
 */
export function whitesRealityCheck(
  returns: Float64Array,
  numStrategies: number,
  numPeriods: number,
  numBootstrap?: number,
  avgBlockSize?: number,
): { p_value: number; best_stat: number; best_index: number };
/**
 * Probability of backtest overfitting by combinatorially symmetric
 * cross-validation. `returns` is a flat row-major `numTrials * numPeriods`
 * matrix of per-period returns, one row per parameter combination;
 * `numBlocks` (default 16, even) is the number of period blocks the
 * C(S, S/2) splits are drawn from. `pbo` is the fraction of splits whose
 * in-sample best ranks below the out-of-sample median, NaN with `splits`
 * 0 when the inputs cannot be split. Deterministic.
 */
export function pboCscv(
  returns: Float64Array,
  numTrials: number,
  numPeriods: number,
  numBlocks?: number,
): { pbo: number; splits: number; mean_oos_rank: number; mean_logit: number };
/**
 * Deflated Sharpe ratio. `returns` is the selected strategy's per-period
 * return series; `trialSharpes` holds the per-period Sharpe ratio of every
 * trial the selection ran over (omit it for no selection: the expected
 * maximum is then 0 and `dsr` is the probabilistic Sharpe ratio against
 * zero). `kurtosis` is 3 for normal returns. Every field is NaN with fewer
 * than two periods or a zero return variance.
 */
export function deflatedSharpe(
  returns: Float64Array,
  trialSharpes?: Float64Array,
): {
  sharpe: number;
  skewness: number;
  kurtosis: number;
  expected_max_sharpe: number;
  dsr: number;
};
/**
 * The neighbourhood of every point of a parameter grid. `values` is
 * row-major over `shape`; a cell's neighbours are the cells whose index
 * differs by at most `radius` (default 1) on every axis, the cell itself
 * excluded, NaN values skipped. Both outputs have one entry per cell;
 * `neighbor_mean` is NaN where a cell has no neighbour.
 */
export function gridNeighborhood(
  values: Float64Array,
  shape: number[],
  radius?: number,
): { neighbor_mean: Float64Array; neighbor_count: Uint32Array };
export function barReturns(
  longSignals: Int8Array,
  shortSignals: Int8Array,
  logReturns: Float64Array,
): Float64Array;
export function tradePnl(
  longSignals: Int8Array,
  shortSignals: Int8Array,
  logReturns: Float64Array,
): Float64Array;

// ── Data I/O ──────────────────────────────────────────────────────────

export interface TradeRecord {
  exchangeTsNs: number;
  recvTsNs: number;
  price: number;
  qty: number;
  tradeId: number;
  symbolId: number;
  side: Side;
}

export interface BboRecord {
  exchangeTsNs: number;
  recvTsNs: number;
  seq: number;
  symbolId: number;
  /** 2 = snapshot, 3 = delta. */
  eventType: number;
  bidPrice: number;
  bidQty: number;
  askPrice: number;
  askQty: number;
}

export interface BookLevel {
  price: number;
  qty: number;
}

export interface BookUpdateRecord {
  exchangeTsNs: number;
  recvTsNs: number;
  seq: number;
  symbolId: number;
  eventType: number;
  bids: BookLevel[];
  asks: BookLevel[];
}

export interface DataWriterStats {
  bytesWritten: number;
  eventsWritten: number;
  segmentsCreated: number;
  tradesWritten: number;
}

export interface DataReaderSummary {
  firstEventNs: number;
  lastEventNs: number;
  totalEvents: number;
  segmentCount: number;
  totalBytes: number;
  durationSeconds: number;
}

export interface DataReaderStats {
  filesRead: number;
  eventsRead: number;
  tradesRead: number;
  bookUpdatesRead: number;
  bytesRead: number;
  crcErrors: number;
  /** Events discarded for arriving past the reorder window. Always 0 when
   *  the reader was constructed with `strictOrdering: true`, which raises
   *  on the first such event instead. */
  lateDropped: number;
  /** Frames whose record type this build does not know. The tape format is
   *  additive, so they are stepped over rather than ending the read. */
  unknownFramesSkipped: number;
}

export interface DataReaderOptions {
  /** Cross-block reorder window in nanoseconds for segments without the
   *  Sorted flag. 0 or omitted keeps the 10s default. */
  reorderWindowNs?: number | bigint;
  /** Stop on the first event that arrives past the reorder window instead
   *  of dropping it and counting it in `stats().lateDropped`. Defaults to
   *  `false`; reproducibility gates set it to `true`. The C ABI carries no
   *  error channel, so here the stop surfaces as `run()` returning `false`
   *  rather than a thrown error; the message (symbol, event type, file,
   *  offset) is written to the engine log. */
  strictOrdering?: boolean;
}

export class DataWriter {
  constructor(outputDir: string, maxSegmentMb?: number, exchangeId?: number);
  writeTrade(
    exchangeTsNs: number | bigint,
    recvTsNs: number | bigint,
    price: number,
    qty: number,
    tradeId: number,
    symbolId: number,
    side: Side,
  ): boolean;
  /** `bids` / `asks` are flat `BigInt64Array`s of interleaved raw int64
   *  `[price, qty, price, qty, ...]` (Price/Quantity scale = 1e8). */
  writeBook(
    exchangeTsNs: bigint,
    recvTsNs: bigint,
    seq: bigint,
    symbolId: number,
    isSnapshot: boolean,
    bids: BigInt64Array,
    asks: BigInt64Array,
  ): boolean;
  flush(): void;
  close(): void;
  stats(): DataWriterStats;
}

export class DataReader {
  /** `fromNs` / `toNs` accept either number or bigint; pass bigint for true ns precision. */
  constructor(dataDir: string, fromNs?: number | bigint, toNs?: number | bigint, options?: DataReaderOptions);
  readonly count: number;
  summary(): DataReaderSummary;
  stats(): DataReaderStats;
  readTrades(max?: number): TradeRecord[];
  readTradesFrom(startTsNs: number | bigint, max?: number): TradeRecord[];
  readBBO(max?: number): BboRecord[];
  readBBOFrom(startTsNs: number | bigint, max?: number): BboRecord[];
  readBookUpdates(): BookUpdateRecord[];
  readBookUpdatesFrom(startTsNs: number | bigint): BookUpdateRecord[];
  /** Streaming aggregator dispatch over the tape in a single
   *  decompression pass. Returns `true` on success; an empty list is a
   *  no-op no-decompression call returning `true`.
   *
   *  The second argument is either an `nThreads` integer (back-compat)
   *  or an options object. `onProgress(pct, tsNs)` fires from inside
   *  the run loop at most once per `progressIntervalMs`; returning
   *  `false` cancels the walk and returns `false` from `run`. Progress
   *  is reported only on the single-thread path (nThreads=1). */
  run(aggregators: Array<EventTypeStatsAggregator | BinCountAggregator | VolumeBinAggregator | OHLCBinAggregator | PeakAggregator | QuantileAggregator | BookSnapshotBinAggregator>, options?: number | DataReaderRunOptions): boolean;
}

export interface DataReaderRunOptions {
  /** Worker count for `DataReader.run`. 0 (default) = auto. 1 = forced
   *  single-thread (required for `onProgress`). >1 = explicit. */
  nThreads?: number;
  /** Called periodically from inside the run loop with the bytes-based
   *  pct (0..1) and the cursor timestamp in nanoseconds. Return `false`
   *  to cancel; any other return value keeps the run going. Only fires
   *  on the single-thread path. */
  onProgress?: (pct: number, tsNs: number) => boolean | void;
  /** Minimum interval between `onProgress` calls in milliseconds.
   *  Defaults to 1000 (1 second). 0 is treated as 1000. */
  progressIntervalMs?: number;
}

// ── Streaming tape aggregator framework ─────────────────────────────

/** Event-type filter applied inside each aggregator. Numeric constants
 *  mirror the C ABI: 1 = Trades, 2 = BooksOnly, 3 = Both. */
export const AggregatorEventFilter: {
  readonly Trades: 1;
  readonly BooksOnly: 2;
  readonly Both: 3;
};
export type AggregatorEventFilter = typeof AggregatorEventFilter[keyof typeof AggregatorEventFilter];

export interface EventTypeStatsRow {
  symbolId: number;
  trades: bigint;
  bookSnapshots: bigint;
  bookDeltas: bigint;
}
export class EventTypeStatsAggregator {
  constructor(eventFilter?: AggregatorEventFilter, symbolFilter?: number[]);
  result(): EventTypeStatsRow[];
}

export interface BinCountRow {
  bucketTsNs: bigint;
  symbolId: number;
  /** 0 = aggregate, 1 = BUY, 2 = SELL. */
  side: number;
  count: bigint;
}
export class BinCountAggregator {
  constructor(bucketNs: bigint | number, bySide?: boolean, bySymbol?: boolean,
              eventFilter?: AggregatorEventFilter, symbolFilter?: number[]);
  result(): BinCountRow[];
}

export interface VolumeBinRow {
  bucketTsNs: bigint;
  symbolId: number;
  side: number;
  qtyRaw: bigint;
}
export class VolumeBinAggregator {
  constructor(bucketNs: bigint | number, bySide?: boolean, bySymbol?: boolean,
              eventFilter?: AggregatorEventFilter, symbolFilter?: number[]);
  result(): VolumeBinRow[];
}

export interface OHLCBinRow {
  bucketTsNs: bigint;
  symbolId: number;
  openRaw: bigint;
  highRaw: bigint;
  lowRaw: bigint;
  closeRaw: bigint;
}
export class OHLCBinAggregator {
  constructor(bucketNs: bigint | number, bySymbol?: boolean,
              eventFilter?: AggregatorEventFilter, symbolFilter?: number[]);
  result(): OHLCBinRow[];
}

export interface PeakEntry {
  count: bigint;
  startNs: bigint;
}
export interface PeakWindowResult {
  windowNs: bigint;
  peaks: PeakEntry[];
}
export class PeakAggregator {
  constructor(windowNsList: Array<bigint | number>, topN?: number,
              oversampleFactor?: number,
              eventFilter?: AggregatorEventFilter, symbolFilter?: number[]);
  result(): PeakWindowResult[];
}

export interface QuantileThreshold {
  quantile: number;
  count: bigint;
}
export interface QuantileWindowResult {
  windowNs: bigint;
  thresholds: QuantileThreshold[];
}
export class QuantileAggregator {
  constructor(windowNsList: Array<bigint | number>, quantiles: number[],
              eventFilter?: AggregatorEventFilter, symbolFilter?: number[]);
  result(): QuantileWindowResult[];
}

/** One (bucket, symbol, depth) row of a reconstructed order-book
 *  snapshot. `level` 0 is top of book; zero price+qty on a side means
 *  no level at that depth. `flags` bit 0 = crossed book (best bid >=
 *  best ask) at this bucket. */
export interface BookSnapshotBinRow {
  bucketTsNs: bigint;
  symbolId: number;
  level: number;
  flags: number;
  bidPriceRaw: bigint;
  bidQtyRaw: bigint;
  askPriceRaw: bigint;
  askQtyRaw: bigint;
}
/** Time-bucketed order-book snapshots reconstructed from book
 *  snapshot/delta events; up to `levels` rows per side per bucket.
 *  Book reconstruction is order-dependent across the whole tape, so
 *  this aggregator requires `run(..., 1)` — parallel runs fail. */
export class BookSnapshotBinAggregator {
  constructor(bucketNs: bigint | number, levels?: number,
              eventFilter?: AggregatorEventFilter, symbolFilter?: number[]);
  result(): BookSnapshotBinRow[];
}

/** Built-in `.lrvx` recorder. Owns a `BinaryLogWriter` on the engine
 *  thread — trade/book events are persisted in C++ on the hot path without
 *  bouncing through JS. Attach via `runner.setMarketDataRecorder(hook)`. */
export interface BinaryLogRecorderHookStats {
  tradesWritten: bigint;
  bookUpdatesWritten: bigint;
  bytesWritten: bigint;
  segmentsCreated: bigint;
  errors: bigint;
}

export class BinaryLogRecorderHook {
  constructor(
    outputDir: string,
    maxSegmentMb?: number,
    exchangeId?: number,
    /** `"none"` (default) or `"lz4"`. */
    compression?: "none" | "lz4",
    /** Stamped into the tape manifest as `metadata.exchange`. Required
     *  for `MergedTapeReader` keying — tapes without an exchange name
     *  will refuse to merge. */
    exchangeName?: string,
    /** Stamped into the tape manifest as `metadata.instrument_type`
     *  (e.g. `"spot"`, `"perpetual"`, `"futures"`, `"option"`). */
    instrumentType?: string,
  );
  addSymbol(
    symbolId: number,
    name: string,
    base?: string,
    quote?: string,
    pricePrecision?: number,
    qtyPrecision?: number,
  ): void;
  flush(): void;
  stats(): BinaryLogRecorderHookStats;
}

/** A single rekeyed symbol entry from `MergedTapeReader.symbolTable()`.
 *  `globalId` is allocated 1..M on construction and is stable for the
 *  lifetime of the reader. Ties between tapes that carry the same
 *  `(exchange, name)` collapse to one entry; tie-breaking follows the
 *  order of the `paths` array passed to the constructor. */
export interface MergedTapeSymbol {
  globalId: number;
  exchange: string;
  name: string;
  pricePrecision: number;
  qtyPrecision: number;
}

export interface MergedTapeStats {
  path: string;
  trades: bigint;
  books: bigint;
  firstEventNs: bigint;
  lastEventNs: bigint;
}

export interface MergedTapeReaderOptions {
  /** Inclusive ns lower bound. Omit / pass `undefined` for no bound. */
  fromNs?: bigint | number;
  /** Inclusive ns upper bound. Omit / pass `undefined` for no bound. */
  toNs?: bigint | number;
  /** Post-rekey global IDs (see `symbolTable()`). Filters the merged
   *  stream to just these symbols. */
  symbols?: number[];
}

/** N-tape merge-on-read. Walks the K-way heap of per-tape readers and
 *  emits a single event stream ordered by `exchange_ts_ns`. Symbol IDs
 *  are rekeyed: each tape's local `symbol_id` is mapped to a `globalId`
 *  keyed by `(metadata.exchange, name)`. Two tapes that carry the same
 *  `(exchange, name)` share a `globalId`; ties on identical
 *  `exchange_ts_ns` break by the order of the `paths` array. Throws if
 *  any two tapes have books for the same `(exchange, name)` with
 *  overlapping time ranges — `MergedTapeReader: invalid paths or
 *  overlapping book streams`. */
export class MergedTapeReader {
  constructor(paths: string[], options?: MergedTapeReaderOptions);
  /** All distinct symbols across the merged tapes. `globalId` is
   *  rekeyed and ties break by `paths` order. */
  symbolTable(): MergedTapeSymbol[];
  /** `[minFirstEventNs, maxLastEventNs]` across all tapes. */
  timeRange(): [bigint, bigint];
  /** Per-input-tape breakdown. `path` order matches the constructor's
   *  `paths` array. */
  perTapeStats(): MergedTapeStats[];
  /** Materialized merged trade stream. `symbolId` is the rekeyed
   *  `globalId`. Use `replayTapes` / streaming APIs for memory-bounded
   *  consumption — this method is for `DataReader` parity. */
  readTrades(): TradeRecord[];
  /** Materialized merged book stream, same shape as
   *  `DataReader.readBookUpdates()`. `symbolId` is the rekeyed
   *  `globalId`. */
  readBooks(): BookUpdateRecord[];
  /** Streaming aggregator dispatch over the merged stream in a single
   *  decompression pass. Same contract as `DataReader.run` — events
   *  carry global-rewritten symbol ids; per-tape provenance is not
   *  surfaced to aggregators.
   *  NOTE: `nThreads` is reserved -- the C ABI ignores it and runs
   *  single-threaded (unlike `DataReader.run`, which honours it). */
  run(aggregators: Array<EventTypeStatsAggregator | BinCountAggregator | VolumeBinAggregator | OHLCBinAggregator | PeakAggregator | QuantileAggregator | BookSnapshotBinAggregator>, nThreads?: number): boolean;
}

export interface Partition {
  partitionId: number;
  fromNs: number;
  toNs: number;
  warmupFromNs: number;
  estimatedEvents: number;
  estimatedBytes: number;
}

export class Partitioner {
  constructor(dataDir: string);
  byTime(numPartitions: number, warmupNs: number | bigint): Partition[];
  byDuration(durationNs: number | bigint, warmupNs: number | bigint): Partition[];
  /** Partition on calendar boundaries. The addon reads a unit *string*;
   *  the numeric form this used to declare was never accepted. */
  byCalendar(unit: 'hour' | 'day' | 'week' | 'month',
             warmupNs: number | bigint): Partition[];
  bySymbol(numPartitions: number): Partition[];
  perSymbol(): Partition[];
  byEventCount(numPartitions: number): Partition[];
}

// ── Segment-level data operations ─────────────────────────────────────

/** Result of `validate(path)` (full single-segment validation). */
export interface SegmentValidationResult {
  valid: boolean;
  headerValid: boolean;
  reportedEventCount: number;
  actualEventCount: number;
  hasIndex: boolean;
  indexValid: boolean;
  tradesFound: number;
  bookUpdatesFound: number;
  crcErrors: number;
  timestampAnomalies: number;
}

/** Result of `validateDataset(dir)`. */
export interface DatasetValidationResult {
  valid: boolean;
  totalSegments: number;
  validSegments: number;
  corruptedSegments: number;
  totalEvents: number;
  totalBytes: number;
  firstTimestamp: number;
  lastTimestamp: number;
}

/** Result of `mergeDir(dir, outputPath)`. */
export interface MergeDirResult {
  success: boolean;
  segmentsMerged: number;
  eventsWritten: number;
  bytesWritten: number;
}

/** Result of `split(inputPath, outputDir, ...)`. */
export interface SplitResult {
  success: boolean;
  segmentsCreated: number;
  eventsWritten: number;
}

/** Result of `exportData(inputPath, outputPath, ...)`. */
export interface ExportResult {
  success: boolean;
  eventsExported: number;
  bytesWritten: number;
}

/** Mode for `split`. `time` = by duration ns, `event_count` = by event count, `size` = by bytes, `symbol` = by symbol id. */
export type SplitMode = "time" | "event_count" | "size" | "symbol";

/** Output format for `exportData`. `binary` (raw segments), `json`, `jsonlines`. */
export type ExportFormat = "binary" | "json" | "jsonlines";

/** Compression codec for `recompress`. `lz4` is the default. */
export type RecompressCodec = "lz4" | "none";

/** Boolean header-only validity check. */
export function validateSegment(path: string): boolean;
/** Full validation: walks every record, checks CRC, indexes, ordering. */
export function validate(path: string): SegmentValidationResult;
/** Recursive dataset validation across all segments under `dir`. */
export function validateDataset(dir: string): DatasetValidationResult;
/** Concatenate one or more input segments into a single output. */
export function mergeSegments(inputPath: string, outputPath: string): boolean;
/** Merge every segment under a directory into one output. */
export function mergeDir(dir: string, outputPath: string): MergeDirResult;
/**
 * Split an input segment by `mode`. `splitArg` interpretation:
 *   - `mode="time"`           → nanoseconds per slice (default 3,600,000,000,000 = 1h)
 *   - `mode="event_count"`    → events per slice
 *   - `mode="size"`           → bytes per slice
 *   - `mode="symbol"`         → ignored (one slice per distinct symbol id)
 *
 * `bufferSize` controls the writer buffer in events (default 1,000,000).
 */
export function split(
  inputPath: string,
  outputDir: string,
  mode?: SplitMode,
  splitArg?: number | bigint,
  bufferSize?: number | bigint,
): SplitResult;
/**
 * Export a segment to JSON / JSON-Lines / re-encoded binary, optionally
 * filtered by `[fromNs, toNs]` (pass 0 for "all").
 */
export function exportData(
  inputPath: string,
  outputPath: string,
  format?: ExportFormat,
  fromNs?: number | bigint,
  toNs?: number | bigint,
): ExportResult;
/** Re-write a segment with a different compression codec. */
export function recompress(inputPath: string, outputPath: string, codec?: RecompressCodec): boolean;
/**
 * Filter a segment to a subset of symbol ids; `symbols` is a `Uint32Array`
 * of numeric ids (NOT names). Returns the number of events written.
 */
export function extractSymbols(inputPath: string, outputPath: string, symbols: Uint32Array): number;
/** Filter a segment to events within `[fromNs, toNs]`. Returns events written. */
export function extractTimeRange(
  inputPath: string,
  outputPath: string,
  fromNs: number | bigint,
  toNs: number | bigint,
): number;
/** Quick summary of a segment (same shape as `DataReader.summary()`). */
export function inspect(path: string): DataReaderSummary;

// ── Indicator graphs ──────────────────────────────────────────────────

/**
 * Node compute callback: receives the graph instance and the current
 * symbol id, returns the node's whole series for that symbol. Read
 * dependencies with `graph.get(symbolId, name)` and raw inputs with
 * `graph.close(symbolId)` etc. The return value is read as a
 * `Float64Array`; anything else throws.
 */
export type IndicatorGraphCompute = (
  graph: IndicatorGraph,
  symbolId: number,
) => Float64Array;

/** Same contract as `IndicatorGraphCompute`, but the graph handed to the
 *  callback is the streaming one (no `get` / `require`; read the current
 *  window via `close` / `high` / `low` / `volume`). */
export type StreamingIndicatorGraphCompute = (
  graph: StreamingIndicatorGraph,
  symbolId: number,
) => Float64Array;

export class IndicatorGraph {
  constructor();
  /** `high` / `low` default to `close`, `volume` to 0. Supplied arrays
   *  must match `close` in length. */
  setBars(
    symbol: number,
    close: Float64Array,
    high?: Float64Array,
    low?: Float64Array,
    volume?: Float64Array,
  ): void;
  /** Register a node under `name`. `dependencies` are the names of
   *  previously-added nodes. */
  addNode(name: string, dependencies: string[], compute: IndicatorGraphCompute): void;
  /** Compute (or return the cached) series for node `name`. Throws if
   *  `name` was never added. */
  require(symbol: number, name: string): Float64Array;
  /** Cached series for node `name`, or `null` if it has not been
   *  computed for `symbol` yet. */
  get(symbol: number, name: string): Float64Array | null;
  close(symbol: number): Float64Array;
  high(symbol: number): Float64Array;
  low(symbol: number): Float64Array;
  volume(symbol: number): Float64Array;
  /** Drop every cached node series for `symbol`. */
  invalidate(symbol: number): void;
  invalidateAll(): void;
}

export class StreamingIndicatorGraph {
  constructor();
  /** Register a node under `name`. `dependencies` are the names of
   *  previously-added nodes. */
  addNode(
    name: string,
    dependencies: string[],
    compute: StreamingIndicatorGraphCompute,
  ): void;
  /** Push one bar. `high` / `low` default to `close`, `volume` to 0. */
  step(symbol: number, close: number, high?: number, low?: number, volume?: number): void;
  current(symbol: number, name: string): number;
  barCount(symbol: number): number;
  reset(symbol: number): void;
  resetAll(): void;
  close(symbol: number): Float64Array;
  high(symbol: number): Float64Array;
  low(symbol: number): Float64Array;
  volume(symbol: number): Float64Array;
}

// ── Forward-looking labels (research-only) ────────────────────────────

export namespace targets {
  /** Future return over `horizon` bars. */
  function future_return(input: Float64Array, horizon: number): Float64Array;
  /** Realized close-to-close volatility over `horizon` bars. */
  function future_ctc_volatility(input: Float64Array, horizon: number): Float64Array;
  /** Linear-fit slope over `horizon` bars. */
  function future_linear_slope(input: Float64Array, horizon: number): Float64Array;
}

// ── Constants ─────────────────────────────────────────────────────────

export const SLIPPAGE_NONE: number;
export const SLIPPAGE_FIXED_TICKS: number;
export const SLIPPAGE_FIXED_BPS: number;
export const SLIPPAGE_VOLUME_IMPACT: number;

export const QUEUE_NONE: number;
export const QUEUE_TOB: number;
export const QUEUE_FULL: number;

export const POSITION_FIFO: 0;
export const POSITION_AVG_COST: 1;

// ── Heatmap renderer ──────────────────────────────────────────────────

export interface HeatmapOptions {
  rowLabels?: ReadonlyArray<string>;
  colLabels?: ReadonlyArray<string>;
  title?: string;
  xAxisName?: string;
  yAxisName?: string;
  metricName?: string;
}

export const report: {
  /** Render a 2D matrix as a self-contained HTML page with an
   *  inline-SVG heatmap. `z` is row-major: z[row][col]. Output is
   *  byte-identical to the Python / Codon binding for the same input
   *  (renderer lives in C++). */
  heatmapHtml(z: ReadonlyArray<ReadonlyArray<number>>,
              opts?: HeatmapOptions): string;
};

// ── Latency models (Phase 1 sampling primitive) ───────────────────────

/** One draw from a latency model. All values are non-negative ns. */
export interface LatencySample {
  feedNs: number;
  orderNs: number;
  fillNs: number;
}

/** Common surface implemented by every concrete latency model. */
export interface LatencyModel {
  feedDelay(): number;
  orderDelay(): number;
  fillDelay(): number;
  sample(): LatencySample;
  /** Re-seed the underlying RNG. No-op for deterministic models. */
  reset(seed?: number): void;
}

export interface ConstantLatencyOptions {
  feedNs?: number;
  orderNs?: number;
  fillNs?: number;
}
export interface GaussianLatencyOptions {
  feedMeanNs?: number;
  feedStddevNs?: number;
  orderMeanNs?: number;
  orderStddevNs?: number;
  fillMeanNs?: number;
  fillStddevNs?: number;
  seed?: number;
}
export interface ExponentialLatencyOptions {
  feedMeanNs?: number;
  orderMeanNs?: number;
  fillMeanNs?: number;
  seed?: number;
}
export interface EmpiricalLatencyOptions {
  feedSamples?: ReadonlyArray<number>;
  orderSamples?: ReadonlyArray<number>;
  fillSamples?: ReadonlyArray<number>;
  seed?: number;
}

/** Returns the same nanoseconds every call. */
export class ConstantLatency implements LatencyModel {
  constructor(opts?: ConstantLatencyOptions);
  feedDelay(): number;
  orderDelay(): number;
  fillDelay(): number;
  sample(): LatencySample;
  reset(seed?: number): void;
}

/** Independent normal samples per component, clamped to non-negative. */
export class GaussianLatency implements LatencyModel {
  constructor(opts?: GaussianLatencyOptions);
  feedDelay(): number;
  orderDelay(): number;
  fillDelay(): number;
  sample(): LatencySample;
  reset(seed?: number): void;
}

/** Exponential per component, parameterised by mean. */
export class ExponentialLatency implements LatencyModel {
  constructor(opts?: ExponentialLatencyOptions);
  feedDelay(): number;
  orderDelay(): number;
  fillDelay(): number;
  sample(): LatencySample;
  reset(seed?: number): void;
}

/** Resample with replacement from observed values. */
export class EmpiricalLatency implements LatencyModel {
  constructor(opts?: EmpiricalLatencyOptions);
  feedDelay(): number;
  orderDelay(): number;
  fillDelay(): number;
  sample(): LatencySample;
  reset(seed?: number): void;
}

// ── Tape diff ─────────────────────────────────────────────────────────

/** One trade-record snapshot used for both sides of a diff. */
export interface TapeDiffTrade {
  exchangeTsNs: number;
  symbolId: number;
  priceRaw: number;
  qtyRaw: number;
  side: number;
}

/** One pair of records that did not match. */
export interface TapeDiffMismatch {
  index: number;
  left: TapeDiffTrade;
  right: TapeDiffTrade;
}

export interface TapeDiffOptions {
  /** Maximum mismatches to record. 0 means no cap. Default 16. */
  maxMismatches?: number;
  /** Allowed symmetric tolerance on exchangeTsNs. Default 0. */
  fieldToleranceNs?: number;
}

export interface TapeDiffResult {
  leftPath: string;
  rightPath: string;
  leftCount: number;
  rightCount: number;
  /** Index of the first mismatch, or null when tapes are equal. */
  firstDivergenceIndex: number | null;
  mismatches: ReadonlyArray<TapeDiffMismatch>;
  equal: boolean;
}

/** Compare two .lrvx directories trade-by-trade. The walk runs in
 *  the C++ engine; result fields match the Python `lrvx.tape.diff_tapes`
 *  output. */
export function tapeDiff(leftPath: string, rightPath: string,
                         opts?: TapeDiffOptions): TapeDiffResult;

// ── DEX amounts: u256 / i256 (256-bit, native BigInt) ─────────────────

/** Round-trip a value through the exact u256 (the DEX curves' amount type),
 *  lossless for any value in [0, 2^256). The boundary type for the bound curves. */
export function u256Roundtrip(value: bigint): bigint;
/** The signed variant (i256), carrying the sign. */
export function i256Roundtrip(value: bigint): bigint;
/** Parse a hex string (with or without a 0x prefix) into a 256-bit BigInt. */
export function u256FromHex(hex: string): bigint;

// ── AMM curves: price a DEX swap (exact, 256-bit) ─────────────────────

/** An opaque AMM pool curve handle (freed automatically when unreachable). */
export type AmmCurveHandle = object;

/** A constant-product pool (Uniswap v2 forks). Reserves are BigInt (native wei). */
export function ammCurveConstantProduct(reserve0: bigint, reserve1: bigint,
                                        feeNum: number, feeDen: number): AmmCurveHandle;
/** A Raydium constant-product pool (Solana), fee rates over a 1e6 denominator. */
export function ammCurveRaydiumCp(reserve0: bigint, reserve1: bigint, tradeFeeRate: number,
                                  creatorFeeRate?: number, creatorFeeOnInput?: boolean): AmmCurveHandle;
/** A Uniswap v3 concentrated-liquidity pool (Q64.96). ticks: [sqrtRatio, liquidityNet][]. */
export function ammCurveUniswapV3(sqrtPriceX96: bigint, liquidity: bigint, feePips: number,
                                  ticks?: Array<[bigint, bigint]>): AmmCurveHandle;
/** The number of tokens in the pool. */
export function ammCurveTokenCount(curve: AmmCurveHandle): number;
/** Exact output for swapping amountIn of token i into token j, without moving the pool. */
export function ammCurveAmountOut(curve: AmmCurveHandle, i: number, j: number, amountIn: bigint): bigint;
/** Apply the swap (moves the pool) and return the output. */
export function ammCurveApplySwap(curve: AmmCurveHandle, i: number, j: number, amountIn: bigint): bigint;
/** The pool's reserve of token i (native wei). */
export function ammCurveBalance(curve: AmmCurveHandle, i: number): bigint;
/** A concentrated-liquidity pool's current sqrtPriceX96, or null for a non-CLMM pool. */
export function ammCurveSqrtPrice(curve: AmmCurveHandle): bigint | null;
/** A concentrated-liquidity pool's active liquidity, or null for a non-CLMM pool. */
export function ammCurveLiquidity(curve: AmmCurveHandle): bigint | null;
/** An independent copy of the pool, for sizing a swap without disturbing the live one. */
export function ammCurveClone(curve: AmmCurveHandle): AmmCurveHandle;

// ── Pool-state tape: replay a recorded DEX pool history ───────────────

/** An opaque pool-state tape being built (freed automatically when unreachable). */
export type PoolTapeHandle = object;
/** An opaque pool-state replay result (freed automatically when unreachable). */
export type PoolReplayHandle = object;

/** Create an empty pool-state tape (a delta log) to build then replay. */
export function poolTapeCreate(): PoolTapeHandle;
/** Write the venue descriptor: a constant-product pool. */
export function poolTapeDescriptorConstantProduct(tape: PoolTapeHandle, feeNum: number,
                                                  feeDen: number, baseDec: number, quoteDec: number): void;
/** Write the venue descriptor: a Raydium constant-product pool. */
export function poolTapeDescriptorRaydiumCp(tape: PoolTapeHandle, tradeFeeRate: number,
                                            creatorFeeRate: number, creatorFeeOnInput: boolean,
                                            baseDec: number, quoteDec: number): void;
/** Write the venue descriptor: a concentrated-liquidity pool (venue: 2=v3, 3=Orca, 4=Raydium CLMM). */
export function poolTapeDescriptorClmm(tape: PoolTapeHandle, venue: number, feePips: number,
                                       baseDec: number, quoteDec: number): void;
/** Write a checkpoint (the two reserves, native wei) at a timestamp. */
export function poolTapeCheckpoint(tape: PoolTapeHandle, tsNs: number, reserve0: bigint,
                                   reserve1: bigint): void;
/** Write a swap delta (baseForQuote sells base into the pool). */
export function poolTapeSwap(tape: PoolTapeHandle, tsNs: number, baseForQuote: boolean,
                             amountIn: bigint): void;
/** Replay the tape through the exact curve, presenting the pair base/quote. */
export function poolTapeReplay(tape: PoolTapeHandle, baseIdx: number, quoteIdx: number,
                               baseDec: number, quoteDec: number): PoolReplayHandle;
/** The number of checkpoints that disagreed with the replayed state. */
export function poolReplayDriftCount(replay: PoolReplayHandle): number;
/** The number of swaps applied. */
export function poolReplayTradeCount(replay: PoolReplayHandle): number;
/** The final pool as an owned AmmCurve handle. */
export function poolReplayCurve(replay: PoolReplayHandle): AmmCurveHandle;

// ── Portfolio risk aggregator ─────────────────────────────────────────

/** Cross-strategy risk limits. Pass any subset; missing fields stay
 *  uncapped. */
export interface PortfolioRiskRules {
  maxDrawdownPct?: number;
  maxDailyLoss?: number;
  maxGrossExposure?: number;
  maxConcentrationPct?: number;
}

export interface PortfolioRiskOptions {
  rules?: PortfolioRiskRules;
  initialEquity?: number;
}

/** One row's writable view; pass any subset to `update`. */
export interface StrategyAccountFields {
  realizedPnl?: number;
  unrealizedPnl?: number;
  fees?: number;
  grossExposure?: number;
  netExposure?: number;
  tradeCount?: number;
}

export interface PortfolioBreach {
  rule: string;
  value: number;
  limit: number;
  detail: string;
}

export interface PortfolioSnapshotJs {
  totalDailyPnl: number;
  totalGrossExposure: number;
  currentEquity: number;
  drawdownPct: number;
  killSwitchActive: boolean;
  breaches: ReadonlyArray<PortfolioBreach>;
  accountCount: number;
}

export class PortfolioRiskAggregator {
  constructor(opts?: PortfolioRiskOptions);
  update(name: string, fields: StrategyAccountFields): void;
  remove(name: string): void;
  resetKillSwitch(): void;
  /** Returns `null` when allowed, or a breach describing the rule
   *  hit. Pre-trade gate; does not mutate state. */
  checkOrder(strategy: string, notional: number, side: string): PortfolioBreach | null;
  snapshot(): PortfolioSnapshotJs;
  totalDailyPnl(): number;
  totalGrossExposure(): number;
  currentEquity(): number;
  drawdownPct(): number;
  killSwitchActive(): boolean;
}

// ── Multi-leg order group ────────────────────────────────────────────

export type OrderGroupPolicyName = 'BestEffort' | 'AllOrNothing' | 'OneSided';
export type OrderGroupStateName =
  | 'Pending' | 'Submitted' | 'PartiallyFilled' | 'Filled'
  | 'Cancelled' | 'Reverting' | 'Failed';
export type LegStateName =
  | 'Pending' | 'Submitted' | 'PartiallyFilled' | 'Filled' | 'Cancelled' | 'Failed';

export const OrderGroupPolicy: Readonly<Record<OrderGroupPolicyName, OrderGroupPolicyName>>;
export const OrderGroupState: Readonly<Record<OrderGroupStateName, OrderGroupStateName>>;

export interface OrderGroupOptions {
  parentSignalId?: number;
  policy?: OrderGroupPolicyName;
}

export interface OrderGroupCancelAction {
  kind: 'cancel';
  legIndex: number;
  orderId: number;
}

export interface OrderGroupRevertAction {
  kind: 'revert';
  legIndex: number;
  symbol: number;
  side: 0 | 1;
  qty: number;
}

export type OrderGroupAction = OrderGroupCancelAction | OrderGroupRevertAction;

export class OrderGroup {
  constructor(opts?: OrderGroupOptions);
  addMarketLeg(symbol: number, side: 0 | 1, qty: number): number;
  addLimitLeg(symbol: number, side: 0 | 1, price: number, qty: number): number;
  legCount(): number;
  legState(legIndex: number): LegStateName;
  legFilled(legIndex: number): number;
  legOrderId(legIndex: number): number;
  recordSubmit(legIndex: number, orderId: number): void;
  recordFill(legIndex: number, cumulativeQty: number): void;
  recordCancel(legIndex: number): void;
  recordFailure(legIndex: number): void;
  recordReplaceAccepted(legIndex: number, newOrderId: number): void;
  recordReplaceRejected(legIndex: number): void;
  findLegByOrderId(orderId: number): number | null;
  state(): OrderGroupStateName;
  recommendedActions(): OrderGroupAction[];
  markActionDispatched(legIndex: number, kind: 'cancel' | 'revert'): void;
  /** Dispatch every queued recommended action through the strategy's
   *  emitCancel / emitMarketBuy / emitMarketSell helpers. Idempotent. */
  autoDispatch(strategy: {
    emitCancel: (orderId: number) => void;
    emitMarketBuy?: (symbol: number, qty: number) => void;
    emitMarketSell?: (symbol: number, qty: number) => void;
  }): number;
  setRiskLimits(opts: {
    maxGrossNotional?: number;
    maxConcentrationPct?: number;
    maxLegQty?: number;
  }): void;
  precheckSubmission(opts?: {
    equity?: number;
    marketRefPrices?: number[];
  }): { denied: boolean; rule: string; detail: string };
  setPairLatencyBudgetNs(budgetNs: number): void;
  pairLatencyDecision(opts: {
    leaderSubmitTsNs: number;
    leaderAckTsNs: number;
    ackReceived?: boolean;
  }): 'wait' | 'submit_follower' | 'cancel_leader';
}

// ── Live queue position estimator ────────────────────────────────

export interface LiveQueueSnapshot {
  orderId: number;
  queueAheadEst: number;
  total: number;
  confidence: number;
  /** Absolute nanosecond clock reading. */
  lastUpdateNs: bigint;
  hiddenVolumeSeen: number;
}

export type HiddenOrderPolicyName =
  | 'ignore'
  | 'trust_trade_flag'
  | 'infer_if_trade_exceeds_visible';

export class LiveQueuePositionEstimator {
  constructor();
  setConfidenceHalfLifeNs(halfLifeNs: number): void;
  setShrinkAttributionFactor(factor: number): void;
  setHiddenOrderPolicy(policy: HiddenOrderPolicyName): void;
  onOrderPlaced(symbol: number, side: 0 | 1, price: number, orderId: number,
                orderQty: number, levelQtyNow: number,
                tsNs?: number | bigint): void;
  onOrderCancelled(orderId: number, tsNs?: number | bigint): void;
  onOrderFilled(orderId: number, cumulativeFill: number,
                tsNs?: number | bigint): void;
  onTrade(symbol: number, price: number, qty: number,
          tsNs?: number | bigint): void;
  onTradeWithFlag(symbol: number, price: number, qty: number,
                  tsNs: number | bigint, isHidden: boolean): void;
  onLevelUpdate(symbol: number, side: 0 | 1, price: number, newQty: number,
                tsNs?: number | bigint): void;
  snapshot(orderId: number, nowNs?: number | bigint): LiveQueueSnapshot | null;
  trackedOrderCount(): number;
}

// ── Bar-close dispatch recorder (cross-binding parity test fixture) ──

export class BarDispatchRecorder {
  constructor();
  /** Register a time-bar timeframe. Returns its slot index (or 8 if full). */
  addTimeIntervalSeconds(seconds: number): number;
  onTrade(symbol: number, price: number, qty: number, tsNs: number): void;
  /** Drain the bus so all bars at the final tied close fire. */
  finalize(): void;
  count(): number;
  /** BarType enum value at index. */
  typeAt(index: number): number;
  /** barTypeParam (nanoseconds for time bars) at index. */
  paramAt(index: number): number;
}

// ── Multi-feed clock ─────────────────────────────────────────────────

export type FeedClockPolicyName = 'WaitForAll' | 'FireOnAny' | 'LeaderFollower';

export const FeedClockPolicy: Readonly<Record<FeedClockPolicyName, FeedClockPolicyName>>;

export interface MultiFeedClockOptions {
  symbols: number[];
  policy?: FeedClockPolicyName;
  timeoutMs?: number;
  leaderSymbol?: number;
  stalenessBudgetMs?: number;
}

export interface FeedClockSnapshot {
  fired: boolean;
  triggeredBy: number;
  /** Symbol id → last-seen exchange-ts in nanoseconds (0 if never). */
  lastTsNs: Record<number, bigint>;
  /** Symbol id → staleness in nanoseconds at the moment of this tick. */
  stalenessNs: Record<number, number>;
}

export class MultiFeedClock {
  constructor(opts: MultiFeedClockOptions);
  tick(tsNs: number | bigint, symbol: number): FeedClockSnapshot;
  reset(): void;
  symbolCount(): number;
}

// ── Execution algorithms (TWAP / VWAP / Iceberg / POV) ────────────────

export interface ExecChildOrder {
  orderId: number;
  timestampNs: number;
  qty: number;
  price: number;
  type: "market" | "limit";
}

interface ExecAlgoCommon {
  targetQty: number;
  side: Side;
  symbol?: number;
  type?: OrderType;
  limitPrice?: number;
}

export interface TWAPOptions extends ExecAlgoCommon {
  durationNs: number;
  sliceCount: number;
  /** A clock reading: takes a `Number` or a `BigInt`. */
  startTimeNs: number | bigint;
}

export interface VWAPOptions extends ExecAlgoCommon {
  /** Array of [timestampNs, volume] rows ordered by time. */
  volumeCurve: ReadonlyArray<[number, number]>;
}

export interface IcebergOptions extends ExecAlgoCommon {
  visibleQty: number;
}

export interface POVOptions extends ExecAlgoCommon {
  participationRate: number;
  minSliceQty?: number;
}

interface ExecAlgo {
  /** Drive the state machine forward and return any newly emitted
   *  child orders. The engine clears its pending buffer before
   *  returning so the next `step` only yields fresh entries. */
  step(nowNs: number): ReadonlyArray<ExecChildOrder>;
  /** User reports a fill on a previously emitted child order. */
  reportFill(qty: number): void;
  /** POV-only. Reports observed market volume. No-op for other algos. */
  observeVolume(qty: number): void;
  submittedQty(): number;
  filledQty(): number;
  remainingQty(): number;
  isDone(): boolean;
}

export class TWAPExecutor implements ExecAlgo {
  constructor(opts: TWAPOptions);
  step(nowNs: number): ReadonlyArray<ExecChildOrder>;
  reportFill(qty: number): void;
  observeVolume(qty: number): void;
  submittedQty(): number;
  filledQty(): number;
  remainingQty(): number;
  isDone(): boolean;
}

export class VWAPExecutor implements ExecAlgo {
  constructor(opts: VWAPOptions);
  step(nowNs: number): ReadonlyArray<ExecChildOrder>;
  reportFill(qty: number): void;
  observeVolume(qty: number): void;
  submittedQty(): number;
  filledQty(): number;
  remainingQty(): number;
  isDone(): boolean;
}

export class IcebergExecutor implements ExecAlgo {
  constructor(opts: IcebergOptions);
  step(nowNs: number): ReadonlyArray<ExecChildOrder>;
  reportFill(qty: number): void;
  observeVolume(qty: number): void;
  submittedQty(): number;
  filledQty(): number;
  remainingQty(): number;
  isDone(): boolean;
}

export class POVExecutor implements ExecAlgo {
  constructor(opts: POVOptions);
  step(nowNs: number): ReadonlyArray<ExecChildOrder>;
  reportFill(qty: number): void;
  observeVolume(qty: number): void;
  submittedQty(): number;
  filledQty(): number;
  remainingQty(): number;
  isDone(): boolean;
}

// ── Delta book compression ────────────────────────────────────────────

export interface DeltaBookLevel {
  priceRaw: number;
  qtyRaw: number;
}

export interface DeltaBookEncodeResult {
  /** false when the encoder emits a full anchor snapshot, true for
   *  a delta against the previous state. */
  isDelta: boolean;
  bids: ReadonlyArray<DeltaBookLevel>;
  asks: ReadonlyArray<DeltaBookLevel>;
}

export interface DeltaBookSnapshot {
  bids: ReadonlyArray<DeltaBookLevel>;
  asks: ReadonlyArray<DeltaBookLevel>;
}

export interface DeltaBookEncoderOptions {
  /** Cadence of full-snapshot anchors. 0 means snapshot-only.
   *  Default 100. */
  anchorEvery?: number;
}

/** Encode a stream of L2 snapshots as anchor snapshots plus deltas. */
export class DeltaBookEncoder {
  constructor(opts?: DeltaBookEncoderOptions);
  encode(symbolId: number, bids: ReadonlyArray<DeltaBookLevel>,
         asks: ReadonlyArray<DeltaBookLevel>): DeltaBookEncodeResult;
  reset(symbolId: number): void;
  resetAll(): void;
}

/** Reverse of DeltaBookEncoder. Reconstructs full snapshots from a
 *  stream of (type, bids, asks) events. type=0 snapshot, type=1 delta. */
export class DeltaBookReplayer {
  constructor();
  apply(type: number, symbolId: number,
        bids: ReadonlyArray<DeltaBookLevel>,
        asks: ReadonlyArray<DeltaBookLevel>): DeltaBookSnapshot;
  reset(symbolId: number): void;
  resetAll(): void;
}

// ---------------------------------------------------------------------------
// .lrvxrun strategy-trace recorder + reader.
// ---------------------------------------------------------------------------

export interface TraceRecorderOptions {
  path: string;
  strategyId?: string;
  strategyHash?: string;
  runStartedNs?: number;
}

export interface TraceTapeRefInput {
  path: string;
  contentHash?: string;
  firstEventNs?: number;
  lastEventNs?: number;
}

export interface TraceSignalInput {
  runTsNs: number;
  feedTsNs?: number;
  signalId?: number;
  flags?: number;
  strengthRaw?: number;
  name?: string;
  symbolIds?: ReadonlyArray<number>;
  payload?: Buffer | string;
}

export interface TraceOrderEventInput {
  runTsNs: number;
  feedTsNs?: number;
  orderId?: number;
  parentSignalId?: number;
  priceRaw?: number;
  qtyRaw?: number;
  symbolId?: number;
  eventKind?: number;
  side?: number;
  orderType?: number;
  flags?: number;
  reason?: string;
}

export interface TraceFillInput {
  runTsNs: number;
  feedTsNs?: number;
  orderId?: number;
  fillId?: number;
  priceRaw?: number;
  qtyRaw?: number;
  feeRaw?: number;
  symbolId?: number;
  side?: number;
  liquidity?: number;
}

export interface TraceSignalRecord {
  runTsNs: number;
  feedTsNs: number;
  signalId: number;
  flags: number;
  strengthRaw: number;
  name: string;
  symbolIds: number[];
  payload: Buffer;
}

export interface TraceOrderEventRecord {
  runTsNs: number;
  feedTsNs: number;
  orderId: number;
  parentSignalId: number;
  priceRaw: number;
  qtyRaw: number;
  symbolId: number;
  eventKind: number;
  side: number;
  orderType: number;
  flags: number;
  reason: string;
}

export interface TraceFillRecord {
  runTsNs: number;
  feedTsNs: number;
  orderId: number;
  fillId: number;
  priceRaw: number;
  qtyRaw: number;
  feeRaw: number;
  symbolId: number;
  side: number;
  liquidity: number;
}

/** Writes signals, order events, and fills into a `.lrvxrun` directory. */
export class TraceRecorder {
  constructor(opts: TraceRecorderOptions);
  addTapeRef(opts: TraceTapeRefInput): void;
  setRunEndedNs(ns: number): void;
  writeSignal(opts: TraceSignalInput): void;
  writeOrderEvent(opts: TraceOrderEventInput): void;
  writeFill(opts: TraceFillInput): void;
  close(): void;
}

/** Reads back a `.lrvxrun` directory written by TraceRecorder. */
export class TraceReader {
  constructor(path: string);
  strategyId(): string;
  strategyHash(): string;
  runStartedNs(): number;
  runEndedNs(): number;
  tapeRefs(): Array<{ path: string }>;
  readAllSignals(): TraceSignalRecord[];
  readAllOrderEvents(): TraceOrderEventRecord[];
  readAllFills(): TraceFillRecord[];
  close(): void;
}

// ── Comfort layers (pure JavaScript, node/lib/) ───────────────────────
//
// `composite` and `dex` are plain-JS namespaces that node/index.js hangs
// off the module (`Object.assign({}, native, { composite, dex })`). They
// are not NAPI exports, so scripts/check_dts_exports.py — which mirrors
// `exports.Set(...)` in node/src — neither knows nor requires them; a
// `namespace` declaration is exempt from that gate.
//
// Implementations: node/lib/composite.js, node/lib/dex.js. The `dex`
// surface is also published standalone as node/lib/dex.d.ts for
// `require('lrvx/lib/dex')`.

/**
 * Composite-condition DSL. Builds multi-symbol / multi-timeframe entry
 * logic as a lazy tree of indicator handles and boolean operators instead
 * of nested `if`s over `lastNClosedBars`.
 *
 * Every node is lazy: it re-reads the bar ring and recomputes on each
 * `isReady()` / `value()` call, so a tree built once in `onStart` stays
 * live for the whole run. Nothing is cached and nothing is mutated.
 *
 * ```js
 * const { when, BAR_TYPE_TIME } = require("@lrvx/lrvx").composite;
 * const fast = when(emit, symbolId, BAR_TYPE_TIME, M5_NS).ema(50);
 * const slow = when(emit, symbolId, BAR_TYPE_TIME, M5_NS).ema(200);
 * const crossUp = fast.gt(slow);
 * if (crossUp.isReady() && crossUp.value()) emit.marketBuy(0.01);
 * ```
 *
 * Mirrors `lrvx.composite` and `quickjs/lrvx/composite.js`.
 */
export namespace composite {
  /** Time bars. `param` is the interval in nanoseconds. */
  const BAR_TYPE_TIME: 0;
  /** Tick bars. `param` is the tick count. */
  const BAR_TYPE_TICK: 1;
  /** Volume bars. `param` is the volume threshold. */
  const BAR_TYPE_VOLUME: 2;
  /** Renko bars. `param` is the brick size. */
  const BAR_TYPE_RENKO: 3;
  /** Range bars. `param` is the range threshold. */
  const BAR_TYPE_RANGE: 4;
  /** Heikin-Ashi bars. `param` is the underlying interval. */
  const BAR_TYPE_HEIKIN_ASHI: 5;
  /** Basis-point range bars. `param` is the range in bps. */
  const BAR_TYPE_BPS_RANGE: 6;

  /** A `BAR_TYPE_*` value. Matches the C++ `BarType` enum. */
  type BarType = 0 | 1 | 2 | 3 | 4 | 5 | 6;

  /** Whatever serves the DSL its bars — in practice the `emit` helper
   *  passed to strategy callbacks, but any object with this one method
   *  works (the test suites drive it with a fake ring). */
  interface BarSource {
    lastNClosedBars(
      symbol: Symbol | number,
      barType: number,
      barTypeParam: number,
      n: number,
    ): ClosedBar[];
  }

  /** A numeric node: an indicator over one (symbol, timeframe). */
  interface Indicator {
    /** True once the ring holds the whole window — `period` bars, or
     *  `period + 1` for `rsi`. */
    isReady(): boolean;
    /** The current value, or `NaN` when `isReady()` is false. */
    value(): number;
    /** `this < operand`. A plain number is lifted into a constant node;
     *  anything with `isReady()` / `value()` is used as-is. */
    lt(operand: Indicator | number): Condition;
    /** `this <= operand`. */
    le(operand: Indicator | number): Condition;
    /** `this > operand`. */
    gt(operand: Indicator | number): Condition;
    /** `this >= operand`. */
    ge(operand: Indicator | number): Condition;
    /** `this === operand` — strict float equality, rarely what you want
     *  on an indicator. */
    eq(operand: Indicator | number): Condition;
    /** `this !== operand`. */
    ne(operand: Indicator | number): Condition;
  }

  /** A boolean node. `and` / `or` / `not` build new nodes; the receiver is
   *  never mutated, so a sub-tree can be shared between conditions. */
  interface Condition {
    /** True once every leaf under this node is ready. */
    isReady(): boolean;
    /** The boolean result. Evaluated even when `isReady()` is false — an
     *  unready leaf yields `NaN`, which makes `lt` / `le` / `gt` / `ge` /
     *  `eq` false but `ne` **true**. Always guard with `isReady()`. */
    value(): boolean;
    /** Logical AND. `other` must already be a node (a plain number is not
     *  lifted here, unlike in the comparison operators). */
    and(other: Condition | Indicator): Condition;
    /** Logical OR. Same operand rule as `and`. */
    or(other: Condition | Indicator): Condition;
    /** Logical NOT. */
    not(): Condition;
  }

  /** A handle bound to one (symbol, timeframe); its methods build the
   *  indicator nodes that read that ring. */
  interface TfHandle {
    /** Simple moving average of the last `period` closes. */
    sma(period: number): Indicator;
    /** Exponential moving average, `alpha = 2 / (period + 1)`, seeded on
     *  the oldest close of the `period`-bar window (not a running EMA
     *  across the whole history). */
    ema(period: number): Indicator;
    /** RSI over `period` close-to-close deltas — a plain mean of gains and
     *  losses, not Wilder smoothing. Needs `period + 1` bars. Returns
     *  `100` when the window has no losses. */
    rsi(period: number): Indicator;
    /** The most recent closed bar's close (a one-bar window). */
    close(): Indicator;
  }

  /** Bind the DSL to one (symbol, timeframe). `barType` is a `BAR_TYPE_*`
   *  constant and `param` that policy's interval / threshold (nanoseconds
   *  for time bars). Nothing is read from `strategy` until a node is
   *  evaluated. */
  function when(
    strategy: BarSource,
    symbol: Symbol | number,
    barType: number,
    param: number,
  ): TfHandle;

  /** One slot of a grid, as returned by `IndicatorGrid.keys()`. */
  interface GridKey {
    symbol: Symbol | number;
    barType: number;
    param: number;
  }

  /** The materialised cross-product of symbols x timeframes. */
  interface IndicatorGrid {
    /** The cell for one slot, or `undefined` when that slot is not in the
     *  grid (the lookup key is `${symbol}|${barType}|${param}`, so the
     *  arguments must match the declaration exactly). */
    get(symbol: Symbol | number, barType: number, param: number): Indicator | undefined;
    /** Every slot in declaration order (symbol-major). A fresh array per
     *  call; the entries are the live key objects. */
    keys(): GridKey[];
    /** `symbols.length * timeframes.length`. */
    size(): number;
  }

  /** Builder returned by `grid()`. Each method materialises a whole grid
   *  of that indicator in one shot. */
  interface GridBuilder {
    sma(period: number): IndicatorGrid;
    ema(period: number): IndicatorGrid;
    rsi(period: number): IndicatorGrid;
    close(): IndicatorGrid;
  }

  /** A grid timeframe: a bare number is a Time-bar interval in
   *  nanoseconds, `[barType, param]` selects any other bar policy. */
  type GridTimeframe = number | [number, number];

  /** Declare the same indicator across every symbol x timeframe slot. */
  function grid(
    strategy: BarSource,
    symbols: ReadonlyArray<Symbol | number>,
    timeframes: ReadonlyArray<GridTimeframe>,
  ): GridBuilder;
}

/**
 * Symbol-aware comfort layer over the exact AMM curves: `Token` / `Amount`
 * / the `Pool` venue classes / `Quote` / `Router` / `arb` / `Tape`. Human
 * amounts go in as `"NUMBER SYMBOL"` strings, the swap math runs in the
 * addon as `bigint` wei, and a mis-routed swap throws instead of returning
 * a quiet zero.
 *
 * This layer only carries the decimals and the token order — every curve
 * evaluation is `ammCurve*` on the top level of this module, so the
 * figures match `lrvx.dex` to the wei.
 *
 * ```js
 * const { dex } = require("@lrvx/lrvx");
 * const weth = new dex.Token("WETH", 18), usdc = new dex.Token("USDC", 6);
 * const pool = new dex.UniswapV2(weth, usdc,
 *   { reserves: ["1000 WETH", "2000000 USDC"], fee: "0.30%" });
 * pool.quote("10 WETH").out.toString();  // "19743.160687 USDC"
 * ```
 */
export namespace dex {
  /** A swap amount: a `"NUMBER SYMBOL"` string (`"10 WETH"`, `_` grouping
   *  allowed) or an `Amount`. A bare number throws — the symbol is what
   *  picks the swap direction. */
  type AmountSpec = string | Amount;

  /** A one-sided quantity where the token is already known (a reserve, a
   *  position value): an `AmountSpec`, or a bare number / bigint / numeric
   *  string in that token's human units. */
  type ReserveSpec = AmountSpec | number | bigint;

  /** An ERC-20 / SPL token: a symbol and its decimal scale. Purely a unit
   *  carrier — no address, no chain. */
  class Token {
    constructor(symbol: string, decimals: number);
    symbol: string;
    decimals: number;
    /** Human units to wei. A `bigint` is taken as whole units and scaled
     *  by `10 ** decimals`; a string or number may carry a fraction, which
     *  is padded or **truncated** to `decimals` places. */
    toWei(human: string | number | bigint): bigint;
    /** wei to an exact human string, trailing zeros stripped. Lossless. */
    toHuman(wei: bigint): string;
    /** `new Amount(this, this.toWei(human))`. */
    amount(human: string | number | bigint): Amount;
  }

  /** A quantity of one token, held exactly as wei. */
  class Amount {
    /** `wei` must be an integer — a fractional number throws (`BigInt()`). */
    constructor(token: Token, wei: bigint | number | string);
    token: Token;
    wei: bigint;
    /** Human magnitude as a float, for arithmetic and display. Lossy above
     *  2^53 — use `humanString` / `wei` when exactness matters. */
    readonly human: number;
    /** Exact human string, no symbol. */
    readonly humanString: string;
    /** `"19743.160687 USDC"`. */
    toString(): string;
    /** Parse `"10 WETH"` against a symbol table. Returns `spec` unchanged
     *  when it is already an `Amount`. Throws when the string carries no
     *  symbol, or when the symbol is absent from `tokens`. */
    static parse(spec: AmountSpec, tokens: Record<string, Token>): Amount;
  }

  /** The result of pricing a swap. */
  class Quote {
    constructor(
      amountIn: Amount,
      out: Amount,
      fee: Amount,
      priceImpact: number,
      avgPrice: number,
    );
    amountIn: Amount;
    out: Amount;
    /** The venue fee, denominated in the **input** token. Computed by the
     *  venue subclass; the base `Pool` always reports zero. */
    fee: Amount;
    /** Impact as a fraction of the mid price (`0.0128` is 1.28%), not a
     *  percentage. Zero when the pool has no mid price, and always zero on
     *  a `Quote` returned by `Pool.swap`. */
    priceImpact: number;
    /** Realized price, output human per input human. Zero for a
     *  zero-sized input. */
    avgPrice: number;
    /** The raw curve output in wei, lossless. */
    readonly exactWei: bigint;
    toString(): string;
  }

  /** One row of `Pool.depth`. */
  interface PoolDepthRow {
    /** The input, formatted (`"10 WETH"`). */
    in: string;
    /** The output, formatted (`"19743.160687 USDC"`). */
    out: string;
    avgPrice: number;
    /** Price impact in **percent**, unlike `Quote.priceImpact`. */
    impactPct: number;
  }

  /** Base class for every venue. Owns the token order and the decimals;
   *  the curve itself lives in the addon. Construct a venue subclass
   *  rather than this directly. */
  class Pool {
    constructor(token0: Token, token1: Token, curve: AmmCurveHandle);
    token0: Token;
    token1: Token;
    /** Venue tag. `"amm"` on the base class; each subclass overrides it. */
    readonly venue: string;
    /** Both reserves in native wei, `[token0, token1]`. */
    balances(): [bigint, bigint];
    /** Price a swap without moving the pool. The input token picks the
     *  direction; `into` names the output side explicitly. Throws when the
     *  amount carries an unknown symbol, when a token is not in this pool,
     *  or when a non-empty pool returns a zero output (the mis-routed-swap
     *  guard). */
    quote(amount: AmountSpec, into?: Token | string): Quote;
    /** Like `quote`, but **applies** the swap — this pool moves. The
     *  returned `Quote` always carries `priceImpact === 0`; clone and
     *  `quote` first if you need the impact. */
    swap(amount: AmountSpec, into?: Token | string): Quote;
    /** Both reserves as `Amount`s, `[token0, token1]`. */
    readonly reserves: [Amount, Amount];
    /** Marginal price of token0 in token1 (WETH priced in USDC), fee
     *  excluded. Read off `sqrtPriceX96` for a concentrated-liquidity
     *  pool, off the reserve ratio otherwise. `0` when the pool is
     *  one-sided or empty. */
    readonly spotPrice: number;
    /** Marginal price of `of` denominated in `in_`. Throws when either
     *  token is not in this pool. */
    price(of: Token, in_: Token): number;
    /** A slippage table: price every size and report the realized price
     *  and impact. Each size is quoted against the unmoved pool. */
    depth(sizes: ReadonlyArray<AmountSpec>): PoolDepthRow[];
    /** An independent copy, native curve included, so a trade can be sized
     *  without disturbing the live pool. */
    clone(): this;
    /** `<UniswapV2 WETH/USDC  px=2000.00  reserves=(1000 WETH, 2000000 USDC)>`. */
    toString(): string;
  }

  interface UniswapV2Options {
    /** `[token0 reserve, token1 reserve]`, in that order. Required — the
     *  options object itself defaults to `{}` in JS, which then throws. */
    reserves: [AmountSpec, AmountSpec];
    /** Swap fee as `"0.30%"` or `0.003`. Default `"0.30%"`. */
    fee?: string | number;
    /** Denominator the pool's integer fee math uses. Default `1000`. */
    feeDen?: number;
  }

  /** A constant-product pool (Uniswap v2 and its forks). */
  class UniswapV2 extends Pool {
    constructor(token0: Token, token1: Token, options: UniswapV2Options);
    readonly venue: "UniswapV2";
  }

  interface RaydiumCpOptions {
    /** `[token0 reserve, token1 reserve]`, in that order. */
    reserves: [AmountSpec, AmountSpec];
    /** Trade fee as `"0.25%"` or `0.0025`, quantised to a 1e6
     *  denominator. Default `"0.25%"`. */
    tradeFee?: string | number;
    /** Creator fee on top, same units. Default `"0%"`. */
    creatorFee?: string | number;
    /** Take the creator fee off the input (`true`) or the output
     *  (`false`). Default `true`. */
    creatorFeeOnInput?: boolean;
  }

  /** A Raydium constant-product pool (Solana). */
  class RaydiumCp extends Pool {
    constructor(token0: Token, token1: Token, options: RaydiumCpOptions);
    readonly venue: "RaydiumCp";
  }

  interface UniswapV3Options {
    /** Current price as Q64.96, straight off `slot0()`. Coerced with
     *  `BigInt()`, so a float throws. */
    sqrtPriceX96: bigint | number | string;
    /** Active liquidity, straight off `liquidity()`. */
    liquidity: bigint | number | string;
    /** Fee as `"0.05%"` or `0.0005`, quantised to pips (1e6). Default
     *  `"0.05%"`. */
    fee?: string | number;
    /** Initialised ticks as `[sqrtRatioX96, liquidityNet]`. Default `[]`,
     *  which prices the pool as if liquidity were flat forever. */
    ticks?: ReadonlyArray<[bigint | number | string, bigint | number | string]>;
  }

  interface UniswapV3FromPriceOptions {
    /** token1 human per token0 human (USDC per WETH). */
    price: number | string;
    liquidity: bigint | number | string;
    /** Default `"0.05%"`. */
    fee?: string | number;
    /** Default `[]`. */
    ticks?: ReadonlyArray<[bigint | number | string, bigint | number | string]>;
  }

  /** A concentrated-liquidity pool (Uniswap v3). */
  class UniswapV3 extends Pool {
    constructor(token0: Token, token1: Token, options: UniswapV3Options);
    readonly venue: "UniswapV3";
    /** The live Q64.96 price. `null` only if the underlying curve is not a
     *  concentrated-liquidity one. */
    readonly sqrtPrice: bigint | null;
    /** The live active liquidity, `null` under the same condition. */
    readonly liquidity: bigint | null;
    /** Build from a human price instead of a raw Q64.96 number. The
     *  conversion runs through a float `sqrt`, so the resulting
     *  `sqrtPriceX96` is rounded — not bit-exact against the chain.
     *  `options` is required; omitting it throws. */
    static fromPrice(
      token0: Token,
      token1: Token,
      options: UniswapV3FromPriceOptions,
    ): UniswapV3;
  }

  /** One row of a `Tape.replay()` result. */
  interface ReplayRow {
    /** The timestamp as handed to `swap` / `checkpoint`. */
    ts: number;
    /** Pool mid price (token1 per token0) after the event. */
    price: number;
    /** Reserves after the event, native wei. */
    reserve0: bigint;
    reserve1: bigint;
    /** Whole-pool value in token1 at `price`. `null` on checkpoint rows. */
    lpValue: number | null;
    /** Pool value against holding the starting mix, as a fraction
     *  (`-0.0006` is -0.06%). `null` on checkpoint rows. */
    il: number | null;
    /** A checkpoint whose asserted reserves disagreed with the replay.
     *  Always `false` on swap rows. */
    drift: boolean;
    /** `true` for a swap row, `false` for a checkpoint row. */
    trade: boolean;
  }

  /** The array `Tape.replay()` returns, with three summary properties hung
   *  off it. Plain JS — no DataFrame dependency. */
  interface ReplayResult extends Array<ReplayRow> {
    /** Checkpoints that disagreed with the replayed state. */
    driftCount: number;
    /** Final reserves after every queued event, native wei. */
    finalReserve0: bigint;
    finalReserve1: bigint;
  }

  /** One entry for `Tape.fromSwaps`: `[ts, amount]` or
   *  `[ts, amount, into]`. */
  type TapeSwapSpec =
    | [number, AmountSpec]
    | [number, AmountSpec, Token | string];

  /** A decoded Uniswap v2 `Swap` log, in `eth_getLogs` shape. */
  interface EvmSwapLog {
    /** ABI-encoded data words in order `amount0In, amount1In, amount0Out,
     *  amount1Out`. A `0x` prefix is optional. */
    data: string;
    /** Used as the row timestamp. A string is parsed as hex. */
    blockNumber?: string | number;
    /** Timestamp fallback when `blockNumber` is absent; `0` when neither
     *  is set. A string is parsed as hex. */
    ts?: string | number;
  }

  /** A pool-state delta log: swaps, optionally with reserve checkpoints,
   *  replayed through the exact curve into a table. */
  class Tape {
    constructor(pool: Pool);
    /** The pool the tape prices against. `replay()` works on a clone, so
     *  this pool is never moved. */
    pool: Pool;
    /** Queue a swap. The amount's token picks the direction; `into` names
     *  the output side explicitly. Returns `this` for chaining. Throws on
     *  an unknown token. */
    swap(ts: number, amount: AmountSpec, into?: Token | string): this;
    /** Queue a swap in raw wei. `baseForQuote` `true` sells token0 into
     *  the pool, `false` sells token1. Returns `this`. */
    swapWei(ts: number, amountInWei: bigint | number | string, baseForQuote: boolean): this;
    /** Assert the pool's reserves at `ts`. On replay a mismatch flags the
     *  row and bumps `driftCount`. Each reserve is resolved against its own
     *  token, so a `"NUMBER SYMBOL"` string must name that side's token.
     *  Returns `this`. */
    checkpoint(ts: number, reserve0: ReserveSpec, reserve1: ReserveSpec): this;
    /** Queue many swaps at once. Returns `this`. */
    fromSwaps(swaps: ReadonlyArray<TapeSwapSpec>): this;
    /** Build a tape from decoded Uniswap v2 `Swap` logs. `pool.token0` /
     *  `pool.token1` must be in on-chain order. A log with no input amount
     *  on either side is skipped. */
    static fromEvmLogs(pool: Pool, swapLogs: ReadonlyArray<EvmSwapLog>): Tape;
    /** Replay every queued event through a clone of the pool, swaps and
     *  checkpoints merged and sorted by `ts` (stable — on a tie every swap
     *  precedes every checkpoint). */
    replay(): ReplayResult;
  }

  /** An LP stake pinned to the pool's price and token mix at construction
   *  time. */
  class LpPosition {
    /** `value` is denominated in **token1** — a `"100000 USDC"` string, an
     *  `Amount` in token1, or a bare number already in token1 units. Omit
     *  it to take the whole pool (`share === 1`). Throws when `value` is an
     *  `Amount` (or symbol string) in token0. */
    constructor(pool: Pool, value?: ReserveSpec);
    /** The pool the position was opened against. */
    pool: Pool;
    /** Fraction of the pool owned, fixed at construction. */
    share: number;
    /** Current worth in token1. `at` prices against another pool — a moved
     *  clone, say — and defaults to the entry pool. */
    value(at?: Pool): number;
    /** What holding the entry mix would be worth now, in token1. */
    hodlValue(at?: Pool): number;
    /** `value(at) / hodlValue(at) - 1`. Negative once the price moves;
     *  `0` when `hodlValue(at)` is not positive. */
    impermanentLoss(at?: Pool): number;
  }

  /** One row of `Router.table`. */
  interface RouterTableRow {
    venue: string;
    /** The output, formatted (`"24.330847... WETH"`). */
    out: string;
    outHuman: number;
    /** Price impact in **percent**. */
    impactPct: number;
  }

  /** Best execution across a set of pools. Routing prices through `quote`,
   *  so no pool is moved. */
  class Router {
    /** Throws when `pools` is empty. */
    constructor(pools: Iterable<Pool> | ArrayLike<Pool>);
    pools: Pool[];
    /** Every pool that can price the swap, as `[pool, quote]` in
     *  construction order. A pool that throws — unknown token, vanishing
     *  output — is skipped silently. */
    quotes(amount: AmountSpec, into?: Token | string): Array<[Pool, Quote]>;
    /** The `[pool, quote]` with the largest output. Throws when no pool
     *  could price the swap. */
    best(amount: AmountSpec, into?: Token | string): [Pool, Quote];
    /** Every pool that could price the swap, ranked by output, best
     *  first. */
    table(amount: AmountSpec, into?: Token | string): RouterTableRow[];
  }

  /** A sized arbitrage between two pools on the same pair. */
  class Arb {
    constructor(size: Amount, profit: Amount, route: [string, string] | null);
    /** The profit-maximising input, denominated in token1. */
    size: Amount;
    /** Net token1 profit, after both pools' fees and slippage. */
    profit: Amount;
    /** `[buyVenue, sellVenue]`, or `null` when there is no edge. */
    route: [string, string] | null;
    /** `profit.wei > 0n`. */
    readonly profitable: boolean;
    toString(): string;
  }

  /** Size the depth-aware arb between two pools on the same pair: spend
   *  token1 on the cheaper pool to buy token0, sell that token0 on the
   *  dearer one. Both `size` and `profit` are in `poolA.token1`.
   *
   *  Neither pool is moved — the search prices through the curve directly.
   *  Throws when the two pools do not trade the same pair. Returns a
   *  zero-size, zero-profit `Arb` with `route === null` when the two
   *  prices agree, when the buy pool holds no token1, or when the optimum
   *  is not profitable. */
  function arb(poolA: Pool, poolB: Pool): Arb;
}
