/** Top of book. A side with no level reads as `null`; a book may be quoted
 *  at exactly zero, so `0` is a price, never "no quote". `midPrice` and
 *  `spread` are numbers only when both sides have a level. */
interface BookSnapshot {
    readonly bidPrice: number | null;
    readonly askPrice: number | null;
    readonly midPrice: number | null;
    readonly spread: number | null;
}

interface SymbolContext {
    readonly symbolId: number;
    readonly symbol: string;
    readonly position: number;
    readonly avgEntryPrice: number;
    /** Quantity-weighted average entry of the open position: an add moves
     *  it, a partial close leaves it, a flip sets it to the flipping fill's
     *  price. `null` when flat. */
    readonly entryPrice: number | null;
    /** Bar callbacks received while holding the current position, the bar
     *  on which it is first seen counting as 1; 0 when flat, 1 again after a
     *  flip. */
    readonly barsInPosition: number;
    readonly lastTradePrice: number;
    readonly lastUpdateNs: bigint;
    readonly lastTouchNs: bigint;
    readonly book: BookSnapshot;
}

interface TradeData {
    readonly symbolId: number;
    readonly symbol: string;
    readonly price: number;
    readonly qty: number;
    readonly side: "buy" | "sell";
    readonly isBuy: boolean;
    readonly timestampNs: bigint;
}

interface BookData {
    readonly symbolId: number;
    readonly symbol: string;
    readonly timestampNs: bigint;
    readonly snapshot: BookSnapshot;
}

interface MarketOrderOpts {
    symbol?: string | number;
    qty: number;
}

interface LimitOrderOpts {
    symbol?: string | number;
    price: number;
    qty: number;
    tif?: "GTC" | "IOC" | "FOK" | "POST_ONLY";
}

interface StopMarketOpts {
    symbol?: string | number;
    side: "buy" | "sell";
    trigger: number;
    qty: number;
}

interface StopLimitOpts {
    symbol?: string | number;
    side: "buy" | "sell";
    trigger: number;
    price: number;
    qty: number;
}

interface TakeProfitMarketOpts {
    symbol?: string | number;
    side: "buy" | "sell";
    trigger: number;
    qty: number;
}

interface TakeProfitLimitOpts {
    symbol?: string | number;
    side: "buy" | "sell";
    trigger: number;
    price: number;
    qty: number;
}

interface TrailingStopOpts {
    symbol?: string | number;
    side: "buy" | "sell";
    offset: number;
    qty: number;
}

interface TrailingStopPercentOpts {
    symbol?: string | number;
    side: "buy" | "sell";
    callbackBps: number;
    qty: number;
}

interface ModifyOpts {
    price?: number;
    qty?: number;
}

declare const enum OrderStatus {
    NEW = 0,
    SUBMITTED = 1,
    ACCEPTED = 2,
    PARTIALLY_FILLED = 3,
    FILLED = 4,
    PENDING_CANCEL = 5,
    CANCELED = 6,
    EXPIRED = 7,
    REJECTED = 8,
    REPLACED = 9,
    PENDING_TRIGGER = 10,
    TRIGGERED = 11,
    TRAILING_UPDATED = 12,
}

/** A closed bar read back from the per-(symbol, barType, param) ring by
 *  `Strategy.lastClosedBar` / `lastNClosedBars`. Prices and volume are
 *  Numbers; the bar's start and end times are BigInt nanoseconds. */
interface ClosedBar {
    readonly open: number;
    readonly high: number;
    readonly low: number;
    readonly close: number;
    readonly volume: number;
    readonly startNs: bigint;
    readonly endNs: bigint;
}

interface StrategyConfig {
    exchange?: string;
    symbols: string[];
}

declare class Strategy {
    constructor(config: StrategyConfig);

    onTrade(ctx: SymbolContext, trade: TradeData): void;
    onBookUpdate(ctx: SymbolContext, book: BookData): void;
    /** The touch changed (price or size on either side, or a side appeared
     *  or emptied): on a top-of-book frame, and after onBookUpdate on a
     *  depth frame that moved it. book.snapshot is the new touch. */
    onTouch(ctx: SymbolContext, book: BookData): void;
    onStart(): void;
    onStop(): void;

    marketBuy(opts: MarketOrderOpts): number;
    marketSell(opts: MarketOrderOpts): number;
    limitBuy(opts: LimitOrderOpts): number;
    limitSell(opts: LimitOrderOpts): number;
    stopMarket(opts: StopMarketOpts): number;
    stopLimit(opts: StopLimitOpts): number;
    takeProfitMarket(opts: TakeProfitMarketOpts): number;
    takeProfitLimit(opts: TakeProfitLimitOpts): number;
    trailingStop(opts: TrailingStopOpts): number;
    trailingStopPercent(opts: TrailingStopPercentOpts): number;
    /** `orderId` is a non-negative integer Number or a BigInt; anything else
     *  is a TypeError and nothing is emitted. */
    cancel(orderId: number | bigint): void;
    cancelAll(symbol?: string | number): void;
    modify(orderId: number | bigint, opts: ModifyOpts): void;
    closePosition(symbol?: string | number): number;

    position(symbol?: string | number): number;
    lastPrice(symbol?: string | number): number;
    /** null when the side is empty; a price below zero, and a price of
     *  exactly 0, are quotes like any other. */
    bestBid(symbol?: string | number): number | null;
    bestAsk(symbol?: string | number): number | null;
    midPrice(symbol?: string | number): number | null;
    /** Quantity-weighted average entry of the symbol's open position, as
     *  `ctx.entryPrice` reads it; null when flat. */
    entryPrice(symbol?: string | number): number | null;
    /** Bar callbacks received while holding the symbol's current position,
     *  as `ctx.barsInPosition` reads it; 0 when flat. */
    barsInPosition(symbol?: string | number): number;
    orderStatus(orderId: number): OrderStatus | -1;

    /** Most recent closed bar of `(symbol, barType, param)`, or null until
     *  one has closed. `barType` is 0=Time, 1=Tick, 2=Volume, 3=Renko,
     *  4=Range, 5=Heikin-Ashi, 6=Bps-range; `param` is the interval in ns for
     *  Time bars, a count for Tick bars, the raw threshold for Volume bars. */
    lastClosedBar(symbol: string | number | undefined, barType: number,
                  param: number | bigint): ClosedBar | null;
    /** At most `n` most recent closed bars of `(symbol, barType, param)`,
     *  oldest first; `[]` until one has closed. Arguments as `lastClosedBar`. */
    lastNClosedBars(symbol: string | number | undefined, barType: number,
                    param: number | bigint, n: number): ClosedBar[];
    /** Bars kept per (symbol, barType, param) ring; 64 by default. */
    readonly barRingCapacity: number;
    /** Set the ring depth; clamped to at least 1. */
    setBarRingCapacity(n: number): void;

    readonly primarySymbol: string;
    readonly hasPosition: boolean;
    readonly symbols: string[];
}

// ============================================================
// Indicators — all 18, unified per-tick + batch API
// ============================================================

// --- Moving Averages ---

declare class SMA {
    constructor(period: number);
    update(value: number): number | null;
    readonly value: number | null;
    readonly ready: boolean;
    /** Updates since construction or the last reset(). */
    readonly count: number;
    reset(): void;
    /** Frees the C handle now; otherwise the finalizer frees it. */
    destroy(): void;
    static compute(data: number[], period: number): number[];
}

declare class EMA {
    constructor(period: number);
    update(value: number): number | null;
    readonly value: number | null;
    readonly ready: boolean;
    /** Updates since construction or the last reset(). */
    readonly count: number;
    reset(): void;
    /** Frees the C handle now; otherwise the finalizer frees it. */
    destroy(): void;
    static compute(data: number[], period: number): number[];
}

declare class RMA {
    constructor(period: number);
    update(value: number): number | null;
    readonly value: number | null;
    readonly ready: boolean;
    /** Updates since construction or the last reset(). */
    readonly count: number;
    reset(): void;
    /** Frees the C handle now; otherwise the finalizer frees it. */
    destroy(): void;
    static compute(data: number[], period: number): number[];
}

declare class DEMA {
    constructor(period: number);
    update(value: number): number | null;
    readonly value: number | null;
    readonly ready: boolean;
    /** Updates since construction or the last reset(). */
    readonly count: number;
    reset(): void;
    /** Frees the C handle now; otherwise the finalizer frees it. */
    destroy(): void;
    static compute(data: number[], period: number): number[];
}

declare class TEMA {
    constructor(period: number);
    update(value: number): number | null;
    readonly value: number | null;
    readonly ready: boolean;
    /** Updates since construction or the last reset(). */
    readonly count: number;
    reset(): void;
    /** Frees the C handle now; otherwise the finalizer frees it. */
    destroy(): void;
    static compute(data: number[], period: number): number[];
}

declare class KAMA {
    constructor(period: number, fast?: number, slow?: number);
    update(value: number): number | null;
    readonly value: number | null;
    readonly ready: boolean;
    /** Updates since construction or the last reset(). */
    readonly count: number;
    reset(): void;
    /** Frees the C handle now; otherwise the finalizer frees it. */
    destroy(): void;
    static compute(data: number[], period: number, fast?: number, slow?: number): number[];
}

// --- Oscillators ---

declare class RSI {
    constructor(period: number);
    update(value: number): number | null;
    readonly value: number | null;
    readonly ready: boolean;
    /** Updates since construction or the last reset(). */
    readonly count: number;
    reset(): void;
    /** Frees the C handle now; otherwise the finalizer frees it. */
    destroy(): void;
    static compute(data: number[], period: number): number[];
}

declare class Skewness {
    constructor(period: number);
    update(value: number): number | null;
    readonly value: number | null;
    readonly ready: boolean;
    /** Updates since construction or the last reset(). */
    readonly count: number;
    reset(): void;
    /** Frees the C handle now; otherwise the finalizer frees it. */
    destroy(): void;
    static compute(data: number[], period: number): number[];
}

declare class Kurtosis {
    constructor(period: number);
    update(value: number): number | null;
    readonly value: number | null;
    readonly ready: boolean;
    /** Updates since construction or the last reset(). */
    readonly count: number;
    reset(): void;
    /** Frees the C handle now; otherwise the finalizer frees it. */
    destroy(): void;
    static compute(data: number[], period: number): number[];
}

declare class RollingZScore {
    constructor(period: number);
    update(value: number): number | null;
    readonly value: number | null;
    readonly ready: boolean;
    /** Updates since construction or the last reset(). */
    readonly count: number;
    reset(): void;
    /** Frees the C handle now; otherwise the finalizer frees it. */
    destroy(): void;
    static compute(data: number[], period: number): number[];
}

declare class ShannonEntropy {
    constructor(period: number, bins?: number);
    update(value: number): number | null;
    readonly value: number | null;
    readonly ready: boolean;
    /** Updates since construction or the last reset(). */
    readonly count: number;
    reset(): void;
    /** Frees the C handle now; otherwise the finalizer frees it. */
    destroy(): void;
    static compute(data: number[], period: number, bins?: number): number[];
}

declare class ParkinsonVol {
    constructor(period: number);
    update(high: number, low: number): number | null;
    readonly value: number | null;
    readonly ready: boolean;
    /** Updates since construction or the last reset(). */
    readonly count: number;
    reset(): void;
    /** Frees the C handle now; otherwise the finalizer frees it. */
    destroy(): void;
    static compute(high: number[], low: number[], period: number): number[];
}

declare class RogersSatchellVol {
    constructor(period: number);
    update(open: number, high: number, low: number, close: number): number | null;
    readonly value: number | null;
    readonly ready: boolean;
    /** Updates since construction or the last reset(). */
    readonly count: number;
    reset(): void;
    /** Frees the C handle now; otherwise the finalizer frees it. */
    destroy(): void;
    static compute(open: number[], high: number[], low: number[], close: number[], period: number): number[];
}

declare class Correlation {
    constructor(period: number);
    update(x: number, y: number): number | null;
    readonly value: number | null;
    readonly ready: boolean;
    /** Updates since construction or the last reset(). */
    readonly count: number;
    reset(): void;
    /** Frees the C handle now; otherwise the finalizer frees it. */
    destroy(): void;
    static compute(x: number[], y: number[], period: number): number[];
}

declare class AutoCorrelation {
    constructor(window: number, lag: number);
    update(value: number): number | null;
    readonly value: number | null;
    readonly ready: boolean;
    /** Updates since construction or the last reset(). */
    readonly count: number;
    reset(): void;
    /** Frees the C handle now; otherwise the finalizer frees it. */
    destroy(): void;
    static compute(data: number[], window: number, lag: number): number[];
}

/** The highest of the last `period` inputs, the current one included; `null`
 *  until `period` inputs are in and while a NaN is in the window. */
declare class Highest {
    constructor(period: number);
    update(value: number): number | null;
    readonly value: number | null;
    readonly ready: boolean;
    /** Updates since construction or the last reset(). */
    readonly count: number;
    reset(): void;
    /** Frees the C handle now; otherwise the finalizer frees it. */
    destroy(): void;
    static compute(data: number[], period: number): number[];
}

/** The lowest of the last `period` inputs; same contract as Highest. */
declare class Lowest {
    constructor(period: number);
    update(value: number): number | null;
    readonly value: number | null;
    readonly ready: boolean;
    /** Updates since construction or the last reset(). */
    readonly count: number;
    reset(): void;
    /** Frees the C handle now; otherwise the finalizer frees it. */
    destroy(): void;
    static compute(data: number[], period: number): number[];
}

declare class Stochastic {
    constructor(kPeriod?: number, dPeriod?: number);
    update(high: number, low: number, close: number): number | null;
    readonly k: number | null;
    readonly d: number | null;
    readonly value: number | null;
    readonly ready: boolean;
    /** Updates since construction or the last reset(). */
    readonly count: number;
    reset(): void;
    /** Frees the C handle now; otherwise the finalizer frees it. */
    destroy(): void;
    static compute(high: number[], low: number[], close: number[], kPeriod?: number, dPeriod?: number): { k: number[]; d: number[] };
}

declare class CCI {
    constructor(period?: number);
    update(high: number, low: number, close: number): number | null;
    readonly value: number | null;
    readonly ready: boolean;
    /** Updates since construction or the last reset(). */
    readonly count: number;
    reset(): void;
    /** Frees the C handle now; otherwise the finalizer frees it. */
    destroy(): void;
    static compute(high: number[], low: number[], close: number[], period?: number): number[];
}

declare class CHOP {
    constructor(period: number);
    update(high: number, low: number, close: number): number | null;
    readonly value: number | null;
    readonly ready: boolean;
    /** Updates since construction or the last reset(). */
    readonly count: number;
    reset(): void;
    /** Frees the C handle now; otherwise the finalizer frees it. */
    destroy(): void;
    static compute(high: number[], low: number[], close: number[], period: number): number[];
}

// --- Trend ---

declare class ATR {
    constructor(period: number);
    update(high: number, low: number, close: number): number | null;
    readonly value: number | null;
    readonly ready: boolean;
    /** Updates since construction or the last reset(). */
    readonly count: number;
    reset(): void;
    /** Frees the C handle now; otherwise the finalizer frees it. */
    destroy(): void;
    static compute(high: number[], low: number[], close: number[], period: number): number[];
}

declare class ADX {
    // Batch-only: there is no streaming ADX in the embedded runtime.
    static compute(high: number[], low: number[], close: number[], period: number): { adx: number[]; plusDi: number[]; minusDi: number[] };
}

declare class Slope {
    constructor(length: number);
    update(value: number): number | null;
    readonly value: number | null;
    readonly ready: boolean;
    /** Updates since construction or the last reset(). */
    readonly count: number;
    reset(): void;
    /** Frees the C handle now; otherwise the finalizer frees it. */
    destroy(): void;
    static compute(data: number[], length: number): number[];
}

declare class MACD {
    constructor(fastPeriod?: number, slowPeriod?: number, signalPeriod?: number);
    update(value: number): number | null;
    readonly line: number | null;
    readonly signal: number | null;
    readonly histogram: number | null;
    readonly value: number | null;
    readonly ready: boolean;
    /** Updates since construction or the last reset(). */
    readonly count: number;
    reset(): void;
    /** Frees the C handle now; otherwise the finalizer frees it. */
    destroy(): void;
    static compute(data: number[], fast?: number, slow?: number, signal?: number): {
        line: number[];
        signal: number[];
        histogram: number[];
    };
}

declare class Bollinger {
    constructor(period?: number, multiplier?: number);
    update(value: number): number | null;
    readonly upper: number | null;
    readonly middle: number | null;
    readonly lower: number | null;
    readonly value: number | null;
    readonly ready: boolean;
    /** Updates since construction or the last reset(). */
    readonly count: number;
    reset(): void;
    /** Frees the C handle now; otherwise the finalizer frees it. */
    destroy(): void;
    static compute(data: number[], period?: number, multiplier?: number): {
        upper: number[];
        middle: number[];
        lower: number[];
    };
}

// --- Volume / cumulative ---
// Batch-only helpers: `static compute` and nothing else. Unlike the
// indicators above they have no streaming instance API (no update /
// value / ready / count / reset).

declare class OBV {
    static compute(close: number[], volume: number[]): number[];
}

declare class VWAP {
    static compute(close: number[], volume: number[], window: number): number[];
}

declare class CVD {
    static compute(open: number[], high: number[], low: number[], close: number[], volume: number[]): number[];
}

/**
 * One OHLCV bar, as produced by `lrvx.loadCsv` and by the `lrvx.*Bars`
 * aggregators. `ts` is the bar's start in nanoseconds, a BigInt on every
 * path -- a Number cannot hold one exactly.
 */
interface Bar {
    readonly ts: bigint;
    readonly open: number;
    readonly high: number;
    readonly low: number;
    readonly close: number;
    readonly volume: number;
}

/** A bar closed by one of the `lrvx.*Bars` aggregators. */
interface AggregatedBar extends Bar {
    readonly buyVolume: number;
    readonly trades: number;
}

// ---- .lrvxrun strategy trace ----

interface TraceRecorderOpts {
    /** The `.lrvxrun` directory to write. */
    path: string;
    strategyId?: string;
    strategyHash?: string;
    runStartedNs?: number | bigint;
}

interface TraceTapeRefOpts {
    path: string;
    contentHash?: string;
    firstEventNs?: number | bigint;
    lastEventNs?: number | bigint;
}

interface TraceSignalOpts {
    runTsNs: number | bigint;
    feedTsNs?: number | bigint;
    /** The whole 64-bit id is written; a BigInt keeps one above 2^53 exact. */
    signalId?: number | bigint;
    /** 1 enter, 2 exit, 4 rebalance. */
    flags?: number;
    strengthRaw?: number | bigint;
    name?: string;
    symbolIds?: number[];
    payload?: string;
}

interface TraceOrderEventOpts {
    runTsNs: number | bigint;
    feedTsNs?: number | bigint;
    orderId?: number | bigint;
    parentSignalId?: number | bigint;
    priceRaw?: number | bigint;
    qtyRaw?: number | bigint;
    symbolId?: number;
    /** 1 submit (the default), 2 cancel, 3 modify, 4 ack, 5 reject, 6 expire. */
    eventKind?: number;
    side?: number;
    orderType?: number;
    /** 1 post-only, 2 reduce-only, 4 IOC. */
    flags?: number;
    reason?: string;
}

interface TraceFillOpts {
    runTsNs: number | bigint;
    feedTsNs?: number | bigint;
    orderId?: number | bigint;
    fillId?: number | bigint;
    priceRaw?: number | bigint;
    qtyRaw?: number | bigint;
    feeRaw?: number | bigint;
    symbolId?: number;
    side?: number;
    /** 0 unknown, 1 maker, 2 taker. */
    liquidity?: number;
}

interface TraceSignalRecord {
    runTsNs: bigint;
    feedTsNs: bigint;
    /** The low 32 bits of the signal's id. */
    signalId: number;
    /** The whole id. For a signal a runner captured it is the order id the
     *  signal asks for. A BigInt, as `orderId` on an order event record is:
     *  an order id is (subscriber id << 32) + n, past what a Number holds
     *  exactly once the subscriber id reaches 2^21. */
    signalId64: bigint;
    flags: number;
    strengthRaw: number;
    name: string;
    symbolIds: number[];
    payload: string;
}

interface TraceOrderEventRecord {
    runTsNs: bigint;
    feedTsNs: bigint;
    orderId: bigint;
    parentSignalId: bigint;
    priceRaw: number;
    qtyRaw: number;
    symbolId: number;
    eventKind: number;
    side: number;
    orderType: number;
    flags: number;
    reason: string;
}

interface TraceFillRecord {
    runTsNs: bigint;
    feedTsNs: bigint;
    orderId: bigint;
    fillId: bigint;
    priceRaw: number;
    qtyRaw: number;
    feeRaw: number;
    symbolId: number;
    side: number;
    liquidity: number;
}

/** Writes signals, order events and fills into a `.lrvxrun` directory. */
interface TraceRecorder {
    addTapeRef(opts: TraceTapeRefOpts): void;
    setRunEndedNs(ns: number | bigint): void;
    writeSignal(opts: TraceSignalOpts): void;
    writeOrderEvent(opts: TraceOrderEventOpts): void;
    writeFill(opts: TraceFillOpts): void;
    /** Finish the segment files and the manifest. */
    close(): void;
    destroy(): void;
}

/** Reads back a `.lrvxrun` directory. Throws when it cannot be opened. */
interface TraceReader {
    strategyId(): string;
    runStartedNs(): bigint;
    runEndedNs(): bigint;
    readAllSignals(): TraceSignalRecord[];
    readAllOrderEvents(): TraceOrderEventRecord[];
    readAllFills(): TraceFillRecord[];
    destroy(): void;
}

// ---- latency models ----

/** A latency model: per-call delays in nanoseconds. */
interface LatencyModel {
    feedDelay(): number;
    orderDelay(): number;
    fillDelay(): number;
    /** One draw of all three delays. */
    sample(): { feedNs: number; orderNs: number; fillNs: number };
    /** Restart the model's random sequence; `seed` 0 or omitted keeps its own. */
    reset(seed?: number): void;
    destroy(): void;
}

interface ConstantLatencyOpts { feedNs?: number; orderNs?: number; fillNs?: number; }
interface GaussianLatencyOpts {
    feedMeanNs?: number; feedStddevNs?: number;
    orderMeanNs?: number; orderStddevNs?: number;
    fillMeanNs?: number; fillStddevNs?: number;
    seed?: number;
}
interface ExponentialLatencyOpts {
    feedMeanNs?: number; orderMeanNs?: number; fillMeanNs?: number; seed?: number;
}
interface EmpiricalLatencyOpts {
    feedSamples?: number[]; orderSamples?: number[]; fillSamples?: number[]; seed?: number;
}

// ---- tape diff ----

interface TapeDiffTrade {
    exchangeTsNs: bigint;
    symbolId: number;
    priceRaw: number;
    qtyRaw: number;
    /** 0 buy, 1 sell. */
    side: number;
}

interface TapeDiffResult {
    leftPath: string;
    rightPath: string;
    leftCount: number;
    rightCount: number;
    /** Index of the first trade that differs; `null` when the tapes agree. */
    firstDivergenceIndex: number | null;
    equal: boolean;
    mismatches: { index: number; left: TapeDiffTrade; right: TapeDiffTrade }[];
}

// ---- delta book ----

interface DeltaBookLevel { priceRaw: number; qtyRaw: number; }

/** Encodes L2 snapshots as an anchor snapshot followed by deltas. */
interface DeltaBookEncoder {
    /** `isDelta` is false for an anchor: `bids` / `asks` are then the whole book. */
    encode(symbolId: number, bids?: DeltaBookLevel[], asks?: DeltaBookLevel[]):
        { isDelta: boolean; bids: DeltaBookLevel[]; asks: DeltaBookLevel[] };
    destroy(): void;
}

/** Rebuilds snapshots from what a DeltaBookEncoder produced. */
interface DeltaBookReplayer {
    /** `type`: 0 for an anchor snapshot, 1 for a delta. Returns the book after it. */
    apply(type: number, symbolId: number, bids?: DeltaBookLevel[], asks?: DeltaBookLevel[]):
        { bids: DeltaBookLevel[]; asks: DeltaBookLevel[] };
    destroy(): void;
}

// ---- bar dispatch recorder ----

/** Records the order in which bars of several timeframes close. */
interface BarDispatchRecorder {
    /** Register a time-bar timeframe; returns its slot index. */
    addTimeIntervalSeconds(seconds: number): number;
    onTrade(symbol: number, price: number, qty: number, tsNs: number | bigint): void;
    /** Drain the bus so every bar at the final tied close fires. */
    finalize(): void;
    count(): number;
    /** The bar type code at `index`. */
    typeAt(index: number): number;
    /** The bar type parameter at `index` (nanoseconds for time bars). */
    paramAt(index: number): number;
    destroy(): void;
}

// ---- execution algos ----

interface ExecChildOrder {
    orderId: number;
    timestampNs: bigint;
    qty: number;
    /** 0 for a market child. */
    price: number;
    type: "market" | "limit";
}

interface ExecAlgoOpts {
    targetQty: number;
    /** Left out: "buy". Any other string is a TypeError. */
    side?: "buy" | "sell";
    symbol?: number;
    /** Left out: "market". Any other string is a TypeError. */
    type?: "market" | "limit";
    limitPrice?: number;
}
interface TWAPOpts extends ExecAlgoOpts {
    durationNs: number | bigint;
    sliceCount: number;
    startTimeNs: number | bigint;
}
interface VWAPOpts extends ExecAlgoOpts {
    /** `[timestampNs, volume]` points of the expected volume curve. */
    volumeCurve?: [number | bigint, number][];
}
interface IcebergOpts extends ExecAlgoOpts { visibleQty: number; }
interface POVOpts extends ExecAlgoOpts { participationRate: number; minSliceQty?: number; }

/** A parent order worked as child orders. Call `step(nowNs)` and send what it returns. */
interface ExecAlgo {
    step(nowNs: number | bigint): ExecChildOrder[];
    reportFill(qty: number): void;
    submittedQty(): number;
    filledQty(): number;
    remainingQty(): number;
    isDone(): boolean;
    destroy(): void;
}
interface POVExecAlgo extends ExecAlgo {
    /** Report market volume observed since the last call. */
    observeVolume(qty: number): void;
}

// ---- portfolio risk ----

interface PortfolioRiskRules {
    maxDrawdownPct?: number;
    maxDailyLoss?: number;
    maxGrossExposure?: number;
    maxConcentrationPct?: number;
}
interface PortfolioRiskBreach { rule: string; value: number; limit: number; detail: string; }
interface PortfolioRiskAccountFields {
    realizedPnl?: number;
    unrealizedPnl?: number;
    fees?: number;
    grossExposure?: number;
    netExposure?: number;
    tradeCount?: number;
}
interface PortfolioRiskAggregator {
    /** Set the named account's fields; the ones left out keep their value. */
    update(name: string, fields?: PortfolioRiskAccountFields): void;
    remove(name: string): void;
    resetKillSwitch(): void;
    /** The rule the order would breach, or `null`. */
    checkOrder(strategy: string, notional: number, side: string): PortfolioRiskBreach | null;
    snapshot(): {
        totalDailyPnl: number;
        totalGrossExposure: number;
        currentEquity: number;
        drawdownPct: number;
        killSwitchActive: boolean;
        breaches: PortfolioRiskBreach[];
        accountCount: number;
    };
    destroy(): void;
}

// ---- indicator graphs ----

/** A node's values for one symbol, computed from the graph it is handed. */
type IndicatorGraphCompute = (graph: IndicatorGraph, symbol: number) => ArrayLike<number>;

interface IndicatorGraph {
    /** `high` / `low` default to `close`, `volume` to zeros. */
    setBars(symbol: number, close: ArrayLike<number>, high?: ArrayLike<number> | null,
            low?: ArrayLike<number> | null, volume?: ArrayLike<number> | null): void;
    /** `deps` are names of nodes added before this one. */
    addNode(name: string, deps: string[] | null | undefined, fn: IndicatorGraphCompute): void;
    /** Compute, or return the cached, series of node `name`. */
    require(symbol: number, name: string): ArrayLike<number>;
    /** The cached series of node `name`, or `null` before it has been computed. */
    get(symbol: number, name: string): ArrayLike<number> | null;
    close(symbol: number): ArrayLike<number>;
    high(symbol: number): ArrayLike<number>;
    low(symbol: number): ArrayLike<number>;
    volume(symbol: number): ArrayLike<number>;
    invalidate(symbol: number): void;
    invalidateAll(): void;
    destroy(): void;
}

/** A node's value for the bar just pushed. */
type StreamingIndicatorGraphCompute =
    (graph: StreamingIndicatorGraph, symbol: number) => number;

interface StreamingIndicatorGraph {
    addNode(name: string, deps: string[] | null | undefined,
            fn: StreamingIndicatorGraphCompute): void;
    /** Push one bar. `high` / `low` default to `close`, `volume` to 0. */
    step(symbol: number, close: number, high?: number, low?: number, volume?: number): void;
    current(symbol: number, name: string): number;
    barCount(symbol: number): number;
    reset(symbol: number): void;
    resetAll(): void;
    close(symbol: number): ArrayLike<number>;
    high(symbol: number): ArrayLike<number>;
    low(symbol: number): ArrayLike<number>;
    volume(symbol: number): ArrayLike<number>;
    destroy(): void;
}

declare const lrvx: {
    TraceRecorder: new (opts: TraceRecorderOpts) => TraceRecorder;
    TraceReader: new (path: string) => TraceReader;
    /** Forward-looking labels, for research only: each reads `horizon` bars
     *  into the future, so feeding one to a live decision is look-ahead. */
    targets: {
        future_return(close: ArrayLike<number>, horizon: number): number[];
        future_ctc_volatility(close: ArrayLike<number>, horizon: number): number[];
        future_linear_slope(close: ArrayLike<number>, horizon: number): number[];
    };
    ConstantLatency: new (opts?: ConstantLatencyOpts) => LatencyModel;
    GaussianLatency: new (opts?: GaussianLatencyOpts) => LatencyModel;
    ExponentialLatency: new (opts?: ExponentialLatencyOpts) => LatencyModel;
    EmpiricalLatency: new (opts?: EmpiricalLatencyOpts) => LatencyModel;
    /** Compare the trades of two `.lrvx` tapes. Throws a TypeError when a
     *  tape cannot be opened. `maxMismatches` bounds the list returned;
     *  timestamps within `fieldToleranceNs` of each other count as equal. */
    tapeDiff(left: string, right: string,
             opts?: { maxMismatches?: number; fieldToleranceNs?: number | bigint }): TapeDiffResult;
    DeltaBookEncoder: new (opts?: { anchorEvery?: number }) => DeltaBookEncoder;
    DeltaBookReplayer: new () => DeltaBookReplayer;
    BarDispatchRecorder: new () => BarDispatchRecorder;
    TWAPExecutor: new (opts: TWAPOpts) => ExecAlgo;
    VWAPExecutor: new (opts: VWAPOpts) => ExecAlgo;
    IcebergExecutor: new (opts: IcebergOpts) => ExecAlgo;
    POVExecutor: new (opts: POVOpts) => POVExecAlgo;
    PortfolioRiskAggregator:
        new (opts?: { rules?: PortfolioRiskRules; initialEquity?: number }) => PortfolioRiskAggregator;
    IndicatorGraph: new () => IndicatorGraph;
    StreamingIndicatorGraph: new () => StreamingIndicatorGraph;
    register(strategy: Strategy): void;
    correlation(x: number[], y: number[]): number;
    profitFactor(pnl: number[]): number;
    winRate(pnl: number[]): number;
    bootstrapCI(data: number[], confidence?: number, samples?: number): { lower: number; median: number; upper: number };
    permutationTest(group1: number[], group2: number[], numPermutations?: number): number;
    /** `false` for a segment that fails its checks and for a file that could
     *  not be read; `lastError()` is `''` for the first and the reason for the
     *  second. */
    validateSegment(path: string): boolean;
    /** `true` only when every input was merged and at least one event was
     *  written. `false` when no segment was merged, and when some inputs
     *  could not be opened while the others were written; `lastError()` says
     *  which. */
    mergeSegments(inputDir: string, outputPath: string): boolean;
    /** `outputPath` is the segment that was written (`''` when none was).
     *  `errors` has one entry for everything that went wrong, among them
     *  `"Cannot open: <path>"` for each input that was skipped while the
     *  others were merged; `success` is `false` whenever it is not empty. */
    mergeDir(inputDir: string, outputDir: string): {
        success: boolean;
        segmentsMerged: number;
        eventsWritten: number;
        bytesWritten: number;
        outputPath: string;
        errors: string[];
    };
    /** Split one segment into several. `mode`: 0 by time (`timeIntervalNs`
     *  per file), 1 by event count (`eventsPerFile`), 2 by size, 3 by symbol.
     *  A zero or omitted interval or count takes the engine's default. */
    splitSegment(inputPath: string, outputDir: string, mode?: number,
                 timeIntervalNs?: number | bigint, eventsPerFile?: number): {
        success: boolean;
        segmentsCreated: number;
        eventsWritten: number;
    };
    /** Export a segment's events. `format`: 0 CSV, 1 JSON, 2 JSON lines,
     *  3 binary. `fromNs` / `toNs` of 0 leave that end open; an empty or
     *  omitted `symbols` keeps every symbol. */
    exportSegment(inputPath: string, outputPath: string, format?: number,
                  fromNs?: number | bigint, toNs?: number | bigint, symbols?: number[]): {
        success: boolean;
        eventsExported: number;
        bytesWritten: number;
    };
    /** Walk every record of a segment. `verifyCrc` and `verifyTimestamps`
     *  are both on when left out; pass `false` to turn one off. */
    validateSegmentFull(path: string, verifyCrc?: boolean, verifyTimestamps?: boolean): {
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
    };
    /** Rewrite a segment with another compression: 0 none (the default),
     *  1 LZ4. `false` when the input cannot be read or the output cannot be
     *  written; `lastError()` says which. */
    recompressSegment(inputPath: string, outputPath: string, compression?: number): boolean;
    /** Copy the events of `symbols` into `outputPath`; returns how many were
     *  written. 0 is also what a failure returns: `lastError()` is `''` when
     *  nothing matched and the reason otherwise. */
    extractSymbols(inputPath: string, outputPath: string, symbols: number[]): number;
    /** Copy the events between `fromNs` and `toNs` into `outputPath`;
     *  returns how many were written, with `lastError()` as for
     *  `extractSymbols`. */
    extractTimeRange(inputPath: string, outputPath: string,
                     fromNs: number | bigint, toNs: number | bigint): number;
    /** `firstTimestamp` / `lastTimestamp` are nanoseconds. */
    validateDataset(dataDir: string): {
        valid: boolean;
        totalSegments: number;
        validSegments: number;
        corruptedSegments: number;
        totalEvents: number;
        totalBytes: number;
        firstTimestamp: bigint;
        lastTimestamp: bigint;
    };
    /** Why the last call that reports failure through its return value
     *  failed; `''` when it did not. */
    lastError(): string;
    /** The release the library was built from (`lrvx_version()`). */
    version(): string;
    /** Read an OHLCV CSV. A ts column in s / ms / us / ns is scaled to ns. */
    loadCsv(path: string): Bar[];

    /** Time bars. `intervalNs` is nanoseconds: 60000000000 is one minute. */
    timeBars(ts: (number | bigint)[], px: number[], qty: number[], sides: (number | boolean)[], intervalNs: number | bigint): AggregatedBar[];
    /** Tick bars. `ticksPerBar` is a trade count. */
    tickBars(ts: (number | bigint)[], px: number[], qty: number[], sides: (number | boolean)[], ticksPerBar: number): AggregatedBar[];
    /** Volume bars. `volumePerBar` is notional per bar. */
    volumeBars(ts: (number | bigint)[], px: number[], qty: number[], sides: (number | boolean)[], volumePerBar: number): AggregatedBar[];
    /** Range bars. `rangeSize` is a high-low price distance. */
    rangeBars(ts: (number | bigint)[], px: number[], qty: number[], sides: (number | boolean)[], rangeSize: number): AggregatedBar[];
    /** Renko bricks. `brickSize` is a price distance; may return more bars than there were trades. */
    renkoBars(ts: (number | bigint)[], px: number[], qty: number[], sides: (number | boolean)[], brickSize: number): AggregatedBar[];
    /** Heikin-Ashi bars. `intervalNs` is nanoseconds, as for timeBars. */
    heikinBars(ts: (number | bigint)[], px: number[], qty: number[], sides: (number | boolean)[], intervalNs: number | bigint): AggregatedBar[];
};

// ============================================================
// Infrastructure classes
// ============================================================

declare class OrderBook {
    constructor(tickSize?: number);
    destroy(): void;
    applySnapshot(bidPrices: number[], bidQtys: number[], askPrices: number[], askQtys: number[]): void;
    applyDelta(bidPrices: number[], bidQtys: number[], askPrices: number[], askQtys: number[]): void;
    bestBid(): number | null;
    bestAsk(): number | null;
    mid(): number | null;
    spread(): number | null;
    getBids(maxLevels?: number): number[][];
    getAsks(maxLevels?: number): number[][];
    isCrossed(): boolean;
    clear(): void;
}

declare class L3Book {
    constructor();
    destroy(): void;
    addOrder(orderId: number, price: number, qty: number, side: "buy" | "sell"): number;
    removeOrder(orderId: number): number;
    modifyOrder(orderId: number, newQty: number): number;
    bestBid(): number | null;
    bestAsk(): number | null;
}

declare type SlippageModelName = "none" | "fixed_ticks" | "fixed_bps" | "volume_impact";
declare type QueueModelName = "none" | "tob" | "full" | "pro_rata" | "pro_rata_with_fifo" | "top_pro_lmm" | "pro_rata_with_priority";

declare class SimulatedExecutor {
    constructor();
    destroy(): void;
    submitOrder(id: number, side: "buy" | "sell", price: number, qty: number, type?: number, symbol?: number): void;
    onBar(symbol: number, closePrice: number): void;
    /** Manual bar path: moves the market to `open` (releasing any order held
     *  from a beginBarCallbackWindow/endBarCallbackWindow pair there), then
     *  walks low -> high -> close. Use this, not onBar, to drive the
     *  executor by hand to the fills BacktestRunner produces on the same
     *  bars. */
    onBarOhlc(symbol: number, open: number, high: number, low: number, close: number): void;
    beginBarCallbackWindow(): void;
    endBarCallbackWindow(): void;
    /** Drop fills and run-scoped state, keeping installed configuration. */
    reset(): void;
    onTrade(symbol: number, price: number, isBuy: boolean): void;
    onTradeQty(symbol: number, price: number, quantity: number, isBuy: boolean): void;
    onBestLevels(symbol: number, bidPrice: number, bidQty: number, askPrice: number, askQty: number): void;
    /** Touch-only frame: moves the touch; deeper depth the executor already
     *  holds survives. `null` means that side of the book is empty. */
    onTopOfBook(symbol: number, bid: { price: number; qty: number } | null, ask: { price: number; qty: number } | null): void;
    advanceClock(timestampNs: bigint | number): void;
    setDefaultSlippage(model: SlippageModelName, ticks?: number, tickSize?: number, bps?: number, impactCoeff?: number): void;
    setSymbolSlippage(symbol: number, model: SlippageModelName, ticks?: number, tickSize?: number, bps?: number, impactCoeff?: number): void;
    setQueueModel(model: QueueModelName, depth?: number): void;
    readonly fillCount: number;
}

declare interface BacktestStats {
    totalTrades: number;
    winningTrades: number;
    losingTrades: number;
    maxConsecutiveWins: number;
    maxConsecutiveLosses: number;
    initialCapital: number;
    finalCapital: number;
    totalPnl: number;
    totalFees: number;
    netPnl: number;
    grossProfit: number;
    grossLoss: number;
    maxDrawdown: number;
    maxDrawdownPct: number;
    winRate: number;
    profitFactor: number;
    avgWin: number;
    avgLoss: number;
    avgWinLossRatio: number;
    avgTradeDurationNs: number;
    medianTradeDurationNs: number;
    maxTradeDurationNs: number;
    sharpeRatio: number;
    sortinoRatio: number;
    calmarRatio: number;
    timeWeightedReturn: number;
    returnPct: number;
    startTimeNs: bigint;
    endTimeNs: bigint;
    /** The positions still open, valued at their mark (see
     *  `BacktestResult.markPrice`): (mark - entry) * qty for a long,
     *  (entry - mark) * qty for a short, no exit fee. `netPnl`,
     *  `finalCapital` and `returnPct` leave it out. */
    unrealizedPnl: number;
    /** `initialCapital + netPnl + unrealizedPnl`. */
    finalEquity: number;
}

/** A position still open: `side` 0 long / 1 short, `quantity` absolute,
 *  `entryPrice` the average entry. */
declare interface OpenPosition {
    symbol: number;
    side: number;
    quantity: number;
    entryPrice: number;
    markPrice: number;
    unrealizedPnl: number;
}

declare interface EquityPoint {
    timestampNs: bigint;
    equity: number;
    drawdownPct: number;
}

declare class BacktestResult {
    constructor(initialCapital?: number, feeRate?: number, usePercentageFee?: boolean,
                fixedFeePerTrade?: number, riskFreeRate?: number, annualizationFactor?: number);
    destroy(): void;
    recordFill(orderId: number, symbol: number, side: "buy" | "sell", price: number,
               quantity: number, timestampNs: bigint | number): void;
    ingestExecutor(executor: SimulatedExecutor): void;
    stats(): BacktestStats;
    /** The price an open position in `symbol` is valued at in `stats()` and
     *  `openPositions()`; the last call wins. A symbol never marked is
     *  valued at its last fill. */
    markPrice(symbol: number, price: number): void;
    /** Positions still open, ascending symbol id. */
    openPositions(): OpenPosition[];
    equityCurve(): EquityPoint[];
    writeEquityCurveCsv(path: string): boolean;
}

declare class PositionTracker {
    constructor(costBasis?: number);
    destroy(): void;
    onFill(symbol: number, side: "buy" | "sell", price: number, qty: number): void;
    position(symbol: number): number;
    avgEntryPrice(symbol: number): number;
    realizedPnl(symbol: number): number;
    totalRealizedPnl(): number;
}

declare class PositionGroupTracker {
    constructor();
    destroy(): void;
    openPosition(orderId: number, symbol: number, side: "buy" | "sell", price: number, qty: number): number;
    closePosition(positionId: number, exitPrice: number): void;
    netPosition(symbol: number): number;
    realizedPnl(symbol: number): number;
    totalRealizedPnl(): number;
    openPositionCount(symbol: number): number;
    prune(): void;
}

declare class OrderTracker {
    constructor();
    destroy(): void;
    onSubmitted(orderId: number, symbol: number, side: "buy" | "sell", price: number, qty: number): boolean;
    onFilled(orderId: number, fillQty: number): boolean;
    onCanceled(orderId: number): boolean;
    isActive(orderId: number): boolean;
    readonly activeCount: number;
    prune(): void;
}

declare interface OrderTraceRow {
    orderId: number;
    seq: number;
    status: number;
    isMaker: boolean;
    tsNs: bigint;
    fillQty: number;
    fillPrice: number;
    queueAhead: number;
    queueTotal: number;
    submittedAtNs: bigint;
    acceptedAtNs: bigint;
    firstFillAtNs: bigint;
    lastFillAtNs: bigint;
    canceledAtNs: bigint;
    rejectedAtNs: bigint;
    triggeredAtNs: bigint;
    expiredAtNs: bigint;
}

declare class OrderJourneyTracer {
    constructor(opts?: {
        maxOrders?: number;
        maxRecordsPerOrder?: number;
        sampleRate?: number;
        sampleSalt?: number;
    });
    destroy(): void;
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

declare class VolumeProfile {
    constructor(tickSize?: number);
    destroy(): void;
    addTrade(price: number, qty: number, isBuy: boolean): void;
    poc(): number;
    valueAreaHigh(): number;
    valueAreaLow(): number;
    totalVolume(): number;
    clear(): void;
}

declare class FootprintBar {
    constructor(tickSize?: number);
    destroy(): void;
    addTrade(price: number, qty: number, isBuy: boolean): void;
    totalDelta(): number;
    totalVolume(): number;
    clear(): void;
}

declare class MarketProfile {
    constructor(tickSize?: number, periodMinutes?: number,
                sessionStartNs?: bigint | number);
    destroy(): void;
    addTrade(timestampNs: bigint | number, price: number, qty: number,
             isBuy: boolean): void;
    poc(): number;
    valueAreaHigh(): number;
    valueAreaLow(): number;
    initialBalanceHigh(): number;
    initialBalanceLow(): number;
    isPoorHigh(): boolean;
    isPoorLow(): boolean;
    clear(): void;
}

declare class CompositeBook {
    constructor();
    destroy(): void;
    applySnapshot(exchange: number, symbol: number, bidPrices: number[], bidQtys: number[],
                  askPrices: number[], askQtys: number[], recvNs: bigint | number): void;
    applyDelta(exchange: number, symbol: number, bidPrices: number[], bidQtys: number[],
               askPrices: number[], askQtys: number[], recvNs: bigint | number): void;
    bestBid(symbol: number): { price: number; quantity: number } | null;
    bestAsk(symbol: number): { price: number; quantity: number } | null;
    hasArbitrage(symbol: number): boolean;
    markStale(exchange: number, symbol: number): void;
    checkStaleness(nowNs: bigint | number, thresholdNs: bigint | number): void;
}

interface WriterStats {
    readonly bytesWritten: number;
    readonly eventsWritten: number;
    readonly segmentsCreated: number;
    readonly tradesWritten: number;
}

interface BinaryLogRecorderStats {
    readonly tradesWritten: number;
    readonly bookUpdatesWritten: number;
    readonly bytesWritten: number;
    readonly segmentsCreated: number;
}

declare class DataWriter {
    /** `maxSegmentSize` is megabytes; 0 or omitted is the 256 MB default. */
    constructor(dir: string, maxSegmentSize?: number, exchangeId?: number);
    destroy(): void;
    writeTrade(timestampNs: bigint | number, exchangeNs: bigint | number,
               price: number, qty: number,
               tradeId: number, symbolId: number, isBuy: boolean): boolean;
    // bidsBuf / asksBuf carry raw int64 levels laid out [price_raw, qty_raw, ...].
    // updateType is the record byte (0 snapshot, 1 delta, 2 top) and decides
    // when given; isSnapshot is the older two-way form.
    writeBook(timestampNs: bigint | number, exchangeNs: bigint | number,
              seqNs: bigint | number, symbolId: number,
              isSnapshot: boolean, bidsBuf: BigInt64Array | null,
              asksBuf: BigInt64Array | null, updateType?: 0 | 1 | 2): boolean;
    flush(): void;
    close(): void;
    stats(): WriterStats;
}

interface TapeTradeRecord {
    readonly exchangeTsNs: bigint;
    readonly recvTsNs: bigint;
    readonly price: number;
    readonly qty: number;
    readonly tradeId: number;
    readonly symbolId: number;
    readonly side: "buy" | "sell";
}

interface TapeBookLevel {
    readonly price: number;
    readonly qty: number;
}

interface TapeBookUpdate {
    readonly exchangeTsNs: bigint;
    readonly recvTsNs: bigint;
    readonly seq: number;
    readonly symbolId: number;
    readonly eventType: number;
    readonly bids: TapeBookLevel[];
    readonly asks: TapeBookLevel[];
}

interface MergedSymbol {
    readonly globalId: number;
    readonly pricePrecision: number;
    readonly qtyPrecision: number;
    readonly exchange: string;
    readonly name: string;
}

interface MergedTapeStats {
    readonly firstEventNs: bigint;
    readonly lastEventNs: bigint;
    readonly trades: bigint;
    readonly books: bigint;
    readonly path: string;
}

interface MergedTimeRange {
    readonly minFirstNs: bigint;
    readonly maxLastNs: bigint;
}

interface MergedBookCount {
    readonly events: bigint;
    readonly levels: bigint;
}

interface MergedTapeReaderOpts {
    paths: string[];
    fromNs?: bigint | number;
    toNs?: bigint | number;
    symbols?: number[];
}

declare class MergedTapeReader {
    constructor(paths: string[], opts?: Omit<MergedTapeReaderOpts, "paths">);
    constructor(opts: MergedTapeReaderOpts);
    destroy(): void;
    readonly symbolCount: number;
    readonly tapeCount: number;
    // Unified symbol table across all tapes, keyed by (exchange, name).
    symbolTable(): MergedSymbol[];
    // Per-tape stats (one entry per input tape, in input order).
    perTapeStats(): MergedTapeStats[];
    // Outer time range across all tapes.
    timeRange(): MergedTimeRange;
    countTrades(): bigint;
    readTrades(maxTrades?: number): TapeTradeRecord[];
    // { events, levels } two-phase count for book updates.
    countBooks(): MergedBookCount;
    readBooks(): TapeBookUpdate[];
}

declare class BinaryLogRecorderHook {
    // exchangeName / instrumentType are stamped into RecordingMetadata so
    // MergedTapeReader can key tapes by (exchange, name).
    constructor(outputDir: string, maxSegmentMb?: number, exchangeId?: number,
                compression?: "none" | "lz4" | number,
                exchangeName?: string, instrumentType?: string);
    destroy(): void;
    addSymbol(symbolId: number, name: string, base?: string, quote?: string,
              pricePrecision?: number, qtyPrecision?: number): void;
    flush(): void;
    stats(): BinaryLogRecorderStats;
    // Private — passed to runner attach API.
    _asRecorderHandle(): unknown;
}

// --- Exit rules (lrvx/exits.js) ---
//
// Each holds one C handle owning the C++ rule of lrvx/strategy/exits.h.
// update() tests the bar against the level resting before it; a hit does
// not disarm and is reported until disarm(). The level reads `null` when
// disarmed. A side other than "long" / "short" throws.

type ExitSide = "long" | "short";

/** A stop `atrMultiple` ATRs from the entry, frozen there. */
declare class AtrStop {
    constructor(atrMultiple: number);
    readonly atrMultiple: number;
    arm(side: ExitSide, entryPrice: number, atr: number): void;
    update(high: number, low: number, atr: number): boolean;
    readonly stop: number | null;
    readonly armed: boolean;
    disarm(): void;
    /** Frees the C handle now; otherwise the finalizer frees it. */
    destroy(): void;
}

/** The stop hangs `atrMultiple` ATRs from the best price of the last
 *  `lookback` bars (0: since the entry) and only moves in the position's
 *  favour. */
declare class ChandelierExit {
    constructor(lookback: number, atrMultiple: number);
    readonly lookback: number;
    readonly atrMultiple: number;
    arm(side: ExitSide, entryPrice: number, atr: number): void;
    update(high: number, low: number, atr: number): boolean;
    readonly stop: number | null;
    readonly armed: boolean;
    disarm(): void;
    destroy(): void;
}

/** A hard stop that moves to the entry once the position is `breakEvenAtr`
 *  ATRs in profit, then trails the best price at `trailAtr` ATRs. */
declare class BreakEvenTrail {
    constructor(hardStopAtr: number, breakEvenAtr: number, trailAtr: number);
    arm(side: ExitSide, entryPrice: number, atr: number): void;
    update(high: number, low: number, atr: number): boolean;
    readonly stop: number | null;
    readonly atBreakEven: boolean;
    readonly armed: boolean;
    disarm(): void;
    destroy(): void;
}

/** A stop at entry * (1 - pct / 100) for a long, entry * (1 + pct / 100)
 *  for a short. Throws unless pct > 0. */
declare class PercentStop {
    constructor(pct: number);
    readonly pct: number;
    arm(side: ExitSide, entryPrice: number): void;
    update(high: number, low: number): boolean;
    readonly stop: number | null;
    readonly armed: boolean;
    disarm(): void;
    destroy(): void;
}

/** A target at entry * (1 + pct / 100) for a long, entry * (1 - pct / 100)
 *  for a short. Throws unless pct > 0. */
declare class TakeProfit {
    constructor(pct: number);
    readonly pct: number;
    arm(side: ExitSide, entryPrice: number): void;
    update(high: number, low: number): boolean;
    readonly target: number | null;
    readonly armed: boolean;
    disarm(): void;
    destroy(): void;
}

/** True from the `maxBars`-th update after arm() on. Throws for 0. */
declare class TimeStop {
    constructor(maxBars: number);
    readonly maxBars: number;
    arm(): void;
    update(): boolean;
    /** Updates since arm(); 0 after disarm(). */
    readonly bars: number;
    readonly armed: boolean;
    disarm(): void;
    destroy(): void;
}

// --- History (lrvx/history.js) ---

/** The last `capacity` values of a series, newest first. */
declare class History {
    /** Throws RangeError unless capacity is a whole number >= 1. */
    constructor(capacity: number);
    push(value: number): void;
    /** 0 is the newest value; throws RangeError outside 0 .. length - 1. */
    get(index: number): number;
    readonly length: number;
    readonly capacity: number;
    readonly full: boolean;
    destroy(): void;
}

/** a was at or below b on the previous step and is strictly above it now.
 *  For a level, pass it as both bPrev and b. A NaN, null or undefined
 *  anywhere is no cross. */
declare function crossedAbove(aPrev: number, a: number, bPrev: number, b: number): boolean;
/** a was at or above b on the previous step and is strictly below it now. */
declare function crossedBelow(aPrev: number, a: number, bPrev: number, b: number): boolean;
