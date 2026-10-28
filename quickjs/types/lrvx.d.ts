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
    symbol?: string;
    qty: number;
}

interface LimitOrderOpts {
    symbol?: string;
    price: number;
    qty: number;
    tif?: "GTC" | "IOC" | "FOK" | "POST_ONLY";
}

interface StopMarketOpts {
    symbol?: string;
    side: "buy" | "sell";
    trigger: number;
    qty: number;
}

interface StopLimitOpts {
    symbol?: string;
    side: "buy" | "sell";
    trigger: number;
    price: number;
    qty: number;
}

interface TakeProfitMarketOpts {
    symbol?: string;
    side: "buy" | "sell";
    trigger: number;
    qty: number;
}

interface TakeProfitLimitOpts {
    symbol?: string;
    side: "buy" | "sell";
    trigger: number;
    price: number;
    qty: number;
}

interface TrailingStopOpts {
    symbol?: string;
    side: "buy" | "sell";
    offset: number;
    qty: number;
}

interface TrailingStopPercentOpts {
    symbol?: string;
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

interface StrategyConfig {
    exchange?: string;
    symbols: string[];
}

declare class Strategy {
    constructor(config: StrategyConfig);

    onTrade(ctx: SymbolContext, trade: TradeData): void;
    onBookUpdate(ctx: SymbolContext, book: BookData): void;
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
    cancel(orderId: number): void;
    cancelAll(symbol?: string): void;
    modify(orderId: number, opts: ModifyOpts): void;
    closePosition(symbol?: string): number;

    position(symbol?: string): number;
    lastPrice(symbol?: string): number;
    /** null when the side is empty; a price below zero, and a price of
     *  exactly 0, are quotes like any other. */
    bestBid(symbol?: string): number | null;
    bestAsk(symbol?: string): number | null;
    midPrice(symbol?: string): number | null;
    orderStatus(orderId: number): OrderStatus | -1;

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
    static compute(data: number[], period: number): number[];
}

declare class EMA {
    constructor(period: number);
    update(value: number): number | null;
    readonly value: number | null;
    readonly ready: boolean;
    static compute(data: number[], period: number): number[];
}

declare class RMA {
    constructor(period: number);
    update(value: number): number | null;
    readonly value: number | null;
    readonly ready: boolean;
    static compute(data: number[], period: number): number[];
}

declare class DEMA {
    constructor(period: number);
    update(value: number): number | null;
    readonly value: number | null;
    readonly ready: boolean;
    static compute(data: number[], period: number): number[];
}

declare class TEMA {
    constructor(period: number);
    update(value: number): number | null;
    readonly value: number | null;
    readonly ready: boolean;
    static compute(data: number[], period: number): number[];
}

declare class KAMA {
    constructor(period: number, fast?: number, slow?: number);
    update(value: number): number | null;
    readonly value: number | null;
    readonly ready: boolean;
    static compute(data: number[], period: number, fast?: number, slow?: number): number[];
}

// --- Oscillators ---

declare class RSI {
    constructor(period: number);
    update(value: number): number | null;
    readonly value: number | null;
    readonly ready: boolean;
    static compute(data: number[], period: number): number[];
}

declare class Stochastic {
    constructor(kPeriod?: number, dPeriod?: number);
    update(high: number, low: number, close: number): number | null;
    readonly k: number | null;
    readonly d: number | null;
    readonly value: number | null;
    readonly ready: boolean;
    static compute(high: number[], low: number[], close: number[], kPeriod?: number, dPeriod?: number): { k: number[]; d: number[] };
}

declare class CCI {
    constructor(period?: number);
    update(high: number, low: number, close: number): number | null;
    readonly value: number | null;
    readonly ready: boolean;
    static compute(high: number[], low: number[], close: number[], period?: number): number[];
}

declare class CHOP {
    constructor(period: number);
    update(high: number, low: number, close: number): number | null;
    readonly value: number | null;
    readonly ready: boolean;
    static compute(high: number[], low: number[], close: number[], period: number): number[];
}

// --- Trend ---

declare class ATR {
    constructor(period: number);
    update(high: number, low: number, close: number): number | null;
    readonly value: number | null;
    readonly ready: boolean;
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

declare const lrvx: {
    register(strategy: Strategy): void;
    correlation(x: number[], y: number[]): number;
    profitFactor(pnl: number[]): number;
    winRate(pnl: number[]): number;
    bootstrapCI(data: number[], confidence?: number, samples?: number): { lower: number; median: number; upper: number };
    permutationTest(group1: number[], group2: number[], numPermutations?: number): number;
    validateSegment(path: string): boolean;
    mergeSegments(inputDir: string, outputPath: string): boolean;
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
declare type QueueModelName = "none" | "tob" | "full";

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
    constructor(dir: string, maxSegmentSize?: number, exchangeId?: number);
    destroy(): void;
    writeTrade(timestampNs: bigint | number, exchangeNs: bigint | number,
               price: number, qty: number,
               tradeId: number, symbolId: number, isBuy: boolean): boolean;
    // bidsBuf / asksBuf carry raw int64 levels laid out [price_raw, qty_raw, ...].
    writeBook(timestampNs: bigint | number, exchangeNs: bigint | number,
              seqNs: bigint | number, symbolId: number,
              isSnapshot: boolean, bidsBuf: BigInt64Array | null,
              asksBuf: BigInt64Array | null): boolean;
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
