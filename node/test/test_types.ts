// node/test/test_types.ts — TypeScript smoke test for index.d.ts.
//
// Touches every exported name so `tsc --noEmit` flags any signature
// drift between the C++ binding and the .d.ts. Companion to
// `scripts/check_dts_exports.py` which catches name-level drift.
//
// Also RUNS: `npx tsc -p test/tsconfig.types.json && node .test-build/test_types.js`
// (see the "Verify Node.js TypeScript authoring layer" CI step). A call that
// type-checks against index.d.ts can still throw against the native addon at
// runtime if the binding's actual accepted types drifted from what the .d.ts
// promises -- tsc alone never catches that, only executing the call does.
// `check()` below asserts on values a mismatched runtime type could produce
// silently (e.g. a declared `bigint` field that is actually `number`).

declare const console: { log(...args: unknown[]): void; error(...args: unknown[]): void };
declare const process: { exitCode?: number; exit(code?: number): never };
// This file compiles with `types: []`, so Node's globals are declared here
// rather than pulled in from @types/node.
declare function setTimeout(handler: () => void, ms: number): { unref(): void };

let _passed = 0;
let _failed = 0;
function check(condition: unknown, msg: string): void {
  if (condition) {
    _passed += 1;
  } else {
    _failed += 1;
    console.error(`  FAIL - ${msg}`);
  }
}
// Calls a declared `number | bigint` overload with a real bigint and
// checks it does not throw -- tsc accepts these calls (they match the
// .d.ts), but several of them used to reject bigint at runtime because
// only some call sites were wired to the number/bigint conversion helper.
function checkAcceptsBigint(fn: () => void, msg: string): void {
  try {
    fn();
    _passed += 1;
  } catch (e) {
    _failed += 1;
    console.error(`  FAIL - ${msg}: threw ${(e as Error).message}`);
  }
}

import * as lrvx from "..";
import {
  // Classes
  ATR,
  AutoCorrelation,
  BacktestResult,
  BacktestRunner,
  Bollinger,
  CCI,
  CHOP,
  CompositeBookMatrix,
  Correlation,
  DEMA,
  BinaryLogRecorderHook,
  DataReader,
  DataWriter,
  EMA,
  Engine,
  FootprintBar,
  IndicatorGraph,
  KAMA,
  Kurtosis,
  L3Book,
  MACD,
  MarketProfile,
  OrderBook,
  OrderTracker,
  ParkinsonVol,
  Partitioner,
  PositionGroupTracker,
  PositionTracker,
  RMA,
  RSI,
  RogersSatchellVol,
  RollingZScore,
  Runner,
  SMA,
  ShannonEntropy,
  SignalBuilder,
  SimulatedExecutor,
  Skewness,
  Slope,
  Stochastic,
  StreamingIndicatorGraph,
  SymbolRegistry,
  TEMA,
  VolumeProfile,
  // Constants
  POSITION_AVG_COST,
  POSITION_FIFO,
  QUEUE_FULL,
  QUEUE_NONE,
  QUEUE_TOB,
  SLIPPAGE_FIXED_BPS,
  SLIPPAGE_FIXED_TICKS,
  SLIPPAGE_NONE,
  SLIPPAGE_VOLUME_IMPACT,
  // Submodule
  targets,
  // Types
  AggregatedBar,
  BarData,
  BboRecord,
  BookUpdateRecord,
  EmitMethods,
  Partition,
  Signal,
  Strategy,
  SymbolContext,
  TradeData,
  TradeRecord,
} from "..";

const arr = new Float64Array([1, 2, 3, 4, 5]);
const tsArr = new BigInt64Array([1n, 2n, 3n, 4n, 5n]);
const ohlcArr = { open: arr, high: arr, low: arr, close: arr, vol: arr };
const ib = new Uint8Array([1, 0, 1, 0, 1]);

// ── Strategy / Runner ──────────────────────────────────────────────────
const registry = new SymbolRegistry();
const sym = registry.addSymbol("binance", "BTCUSDT", 0.01);
const _symId: number = sym.id;
const _symName: string = sym.name;
const _symCount: number = registry.symbolCount();

const strategy: Strategy = {
  symbols: [sym, 1],
  onStart() {},
  onStop() {},
  onTrade(ctx: SymbolContext, trade: TradeData, emit: EmitMethods) {
    void ctx.position;
    void trade.price;
    // TradeData.timestampNs is declared bigint -- a plain `number` here
    // would silently lose precision on a real nanosecond timestamp.
    check(typeof trade.timestampNs === "bigint", "TradeData.timestampNs is bigint at runtime");
    // The order-emitting functions return the order id.
    const bought: number = emit.marketBuy(0.1);
    const sold: number = emit.marketSell(0.1);
    const bid: number = emit.limitBuy(100, 0.1);
    const ask: number = emit.limitSell(100, 0.1);
    check(typeof bought === "number" && typeof sold === "number" && typeof bid === "number" &&
          typeof ask === "number", "emit.* returns the order id as a number at runtime");
    emit.modify(bid, 99, 0.1);
    emit.cancel(ask);
    const closed: number = emit.closePosition();
    void closed;
  },
  onBar(_ctx, bar: BarData, _emit) {
    void bar.open;
    void bar.startTimeNs;
    void bar.barType;
  },
  onBookUpdate(_ctx, _emit) {},
};

const runner = new Runner(registry, (sig: Signal) => {
  void sig.side;
  void sig.orderType;
  void sig.orderId;
});
runner.addStrategy(strategy);
runner.start();
checkAcceptsBigint(() => runner.onTrade(sym, 100, 1, true, 0n), "Runner.onTrade accepts bigint ts");
runner.onTrade(1, 100, 1, false, 0);
checkAcceptsBigint(() => runner.onBookSnapshot(sym, arr, arr, arr, arr, 0n), "Runner.onBookSnapshot accepts bigint ts and Float64Array levels");
checkAcceptsBigint(() => runner.onBookTop(sym, { price: 100.02, qty: 4 }, null, 0n), "Runner.onBookTop accepts bigint ts and a null side");
runner.onBar(sym, { open: 1, high: 2, low: 0.5, close: 1.5 });
runner.stop();

const threadedRunner = new Runner(registry, (_s) => {}, true);
threadedRunner.start();
threadedRunner.stop();

// ── Engine + SignalBuilder ─────────────────────────────────────────────
const engine = new Engine();
const engine2 = new Engine(100000, 0.0001);
void engine2;
engine.loadOhlcv(
  { ts: arr, open: arr, high: arr, low: arr, close: arr, volume: arr },
  "BTCUSDT",
);
engine.loadOhlcv({ ts: arr, open: arr, high: arr, low: arr, close: arr, volume: arr });
engine.resample("BTCUSDT", "BTCUSDT_1h", "1h");
const _bc: number = engine.barCount();
const _bcSym: number = engine.barCount("BTCUSDT");
void _bcSym;
const _ts: BigInt64Array = engine.ts();
const _opens: Float64Array = engine.open("BTCUSDT");
const _highs: Float64Array = engine.high();
const _lows: Float64Array = engine.low();
const _closes: Float64Array = engine.close();
const _vols: Float64Array = engine.volume();
const _syms: string[] = engine.symbols;

const signals = new SignalBuilder();
signals.buy(0, 0.1);
signals.sell(1, 0.1, "BTCUSDT");
signals.limitBuy(2, 100, 0.1);
signals.limitSell(3, 100, 0.1, "BTCUSDT").buy(4, 0.1);
const _sigLen: number = signals.length;
signals.clear();

const stats = engine.run(signals);
const _net: number = stats.netPnl;
const _sharpe: number = stats.sharpeRatio;

// ── Backtest classes ───────────────────────────────────────────────────
const bt = new BacktestRunner(registry, 0.0001, 100000);
bt.setStrategy(strategy);
const _btStats = bt.runOhlcv(tsArr, arr, "BTCUSDT");
void _btStats.totalTrades;

const exec = new SimulatedExecutor();
exec.submitOrder(1, "buy", 100, 0.1, "limit", 1);
exec.cancelOrder(1);
exec.cancelAll(1);
exec.onBar(1, 100);
exec.onTrade(1, 100, true);
exec.onTopOfBook(1, { price: 100.02, qty: 4 }, null);
checkAcceptsBigint(() => exec.advanceClock(1n), "SimulatedExecutor.advanceClock accepts bigint ts");
exec.setDefaultSlippage("fixed_bps", 0, 0, 2.0, 0);
exec.setDefaultSlippage(SLIPPAGE_FIXED_BPS, 0, 0, 2.0, 0);
exec.setQueueModel("tob", 1);
exec.setQueueModel(QUEUE_TOB, 1);
const _fc: number = exec.fillCount;

const result = new BacktestResult(100000, 0.0001);
checkAcceptsBigint(() => result.recordFill(1, 1, "buy", 100, 0.1, 0n), "BacktestResult.recordFill accepts bigint ts");
result.ingestExecutor(exec);
const _resStats = result.stats();
void _resStats.profitFactor;

// ── Streaming indicators ───────────────────────────────────────────────
function exerciseSingle<T extends { update: (v: number) => number | null; value: number; ready: boolean; reset: () => void; compute: (a: Float64Array) => Float64Array }>(ind: T) {
  const v = ind.update(1);
  void (v === null ? null : v + 1);
  void ind.value;
  void ind.ready;
  ind.reset();
  void ind.compute(arr);
}

exerciseSingle(new SMA(3));
exerciseSingle(new EMA(3));
exerciseSingle(new RMA(3));
exerciseSingle(new RSI(14));
exerciseSingle(new DEMA(3));
exerciseSingle(new TEMA(3));
exerciseSingle(new KAMA(10, 2, 30));
exerciseSingle(new Slope(5));
exerciseSingle(new Skewness(20));
exerciseSingle(new Kurtosis(20));
exerciseSingle(new RollingZScore(20));

const se = new ShannonEntropy(20, 8);
void se.update(1);
void se.compute(arr);
const ac = new AutoCorrelation(20, 1);
void ac.update(1);

const atrInd = new ATR(14);
void atrInd.update(1, 0.5, 0.8);
void atrInd.compute(arr, arr, arr);

const cciInd = new CCI(14);
void cciInd.update(1, 0.5, 0.8);

const chopInd = new CHOP(14);
void chopInd.update(1, 0.5, 0.8);
void chopInd.compute(arr, arr, arr);

const stoch = new Stochastic(14, 3);
const stochOut = stoch.update(1, 0.5, 0.8);
void (stochOut.k === null ? 0 : stochOut.k);
const stochBatch = stoch.compute(arr, arr, arr);
void stochBatch.k;
void stochBatch.d;

const pv = new ParkinsonVol(14);
void pv.update(1, 0.5);
const rsv = new RogersSatchellVol(14);
void rsv.update(1, 1.2, 0.8, 1);
const corrI = new Correlation(20);
void corrI.update(1, 2);

const macdInd = new MACD(12, 26, 9);
const macdOut = macdInd.update(1);
void (macdOut.line === null ? 0 : macdOut.line);
const macdBatch = macdInd.compute(arr);
void macdBatch.line;
void macdBatch.signal;
void macdBatch.histogram;

const bb = new Bollinger(20, 2);
const bbOut = bb.update(1);
void (bbOut.upper === null ? 0 : bbOut.upper);
const bbBatch = bb.compute(arr);
void bbBatch.upper;
void bbBatch.middle;
void bbBatch.lower;

// ── Batch indicator functions ──────────────────────────────────────────
void lrvx.sma(arr, 3);
void lrvx.ema(arr, 3);
void lrvx.rma(arr, 3);
void lrvx.rsi(arr, 14);
void lrvx.dema(arr, 3);
void lrvx.tema(arr, 3);
void lrvx.kama(arr, 10, 2, 30);
void lrvx.slope(arr, 5);
void lrvx.skewness(arr, 20);
void lrvx.kurtosis(arr, 20);
void lrvx.rolling_zscore(arr, 20);
void lrvx.shannon_entropy(arr, 20, 8);
void lrvx.autocorrelation(arr, 20, 1);
const adfV = lrvx.adf(arr, 1, "c");
check(typeof adfV.testStat === "number", "adf() returns { testStat, pValue, usedLag }");
void adfV.pValue;
void adfV.usedLag;
void lrvx.chop(arr, arr, arr, 14);
void lrvx.atr(arr, arr, arr, 14);
{
  // Mismatched array lengths used to be read past the short array's end
  // (denormals on a small input, SIGSEGV on a large one) instead of
  // being rejected. `high` is length 5, `low` is length 1.
  const shortArr = new Float64Array([1]);
  let threw = false;
  try {
    lrvx.atr(arr, shortArr, arr, 14);
  } catch {
    threw = true;
  }
  check(threw, "lrvx.atr rejects mismatched array lengths instead of reading out of bounds");
}
void lrvx.cci(arr, arr, arr, 14);
void lrvx.parkinson_vol(arr, arr, 14);
void lrvx.rogers_satchell_vol(arr, arr, arr, arr, 14);
const adxOut = lrvx.adx(arr, arr, arr, 14);
void adxOut.plusDi;
void adxOut.minusDi;
const bbBatch2 = lrvx.bollinger(arr, 20, 2);
void bbBatch2.middle;
const macdBatch2 = lrvx.macd(arr, 12, 26, 9);
void macdBatch2.histogram;
const stochBatch2 = lrvx.stochastic(arr, arr, arr, 14, 3);
void stochBatch2.k;
void lrvx.obv(arr, arr);
void lrvx.vwap(arr, arr, 20);
void lrvx.cvd(arr, arr, arr, arr, arr);
const indNames: string[] = lrvx.list_indicators();
void indNames;

// ── Aggregators ───────────────────────────────────────────────────────
const bars: AggregatedBar[] = lrvx.aggregateTimeBars(arr, arr, arr, ib, 60);
void bars[0]?.open;
void lrvx.aggregateTickBars(arr, arr, arr, ib, 100);
void lrvx.aggregateVolumeBars(arr, arr, arr, ib, 1000);
void lrvx.aggregateRangeBars(arr, arr, arr, ib, 1.0);
void lrvx.aggregateRenkoBars(arr, arr, arr, ib, 1.0);
void lrvx.aggregateHeikinAshiBars(arr, arr, arr, ib, 60);

// ── Books ─────────────────────────────────────────────────────────────
const ob = new OrderBook(0.01);
ob.applySnapshot(arr, arr, arr, arr);
{
  // Same class of bug as the indicator check above: bidQty shorter than
  // bidPrice used to be read at bidPrice's length.
  const shortArr = new Float64Array([1]);
  let threw = false;
  try {
    ob.applySnapshot(arr, shortArr, arr, arr);
  } catch {
    threw = true;
  }
  check(threw, "OrderBook.applySnapshot rejects mismatched bid price/qty lengths");
}
ob.applyDelta(arr, arr, arr, arr);
const _bid: number | null = ob.bestBid();
const _ask: number | null = ob.bestAsk();
void ob.mid();
void ob.spread();
void ob.getBids(5);
void ob.getAsks(5);
void ob.isCrossed();
ob.clear();

const l3 = new L3Book();
void l3.addOrder(1, 100, 1, "buy");
void l3.removeOrder(1);
void l3.modifyOrder(1, 2);
void l3.bidAtPrice(100);
void l3.askAtPrice(100);

const cbm = new CompositeBookMatrix();
cbm.applySnapshot(
  1,
  1,
  new Float64Array([100.0]),
  new Float64Array([1.5]),
  new Float64Array([100.02]),
  new Float64Array([0.5]),
  1000n,
);
cbm.applyDelta(
  1,
  1,
  new Float64Array([100.01]),
  new Float64Array([2.0]),
  new Float64Array([]),
  new Float64Array([]),
);
const bidQ = cbm.bestBid(1);
if (bidQ) {
  void bidQ.price;
  void bidQ.qty;
}
void cbm.bestAsk(1);
void cbm.hasArbitrage(1);
cbm.markStale(1, 1);
cbm.checkStaleness(0n, 1n);

// ── Position tracking ─────────────────────────────────────────────────
const pt = new PositionTracker(POSITION_FIFO);
pt.onFill(1, "buy", 100, 0.1);
void pt.position(1);
void pt.avgEntryPrice(1);
void pt.realizedPnl(1);
void pt.totalRealizedPnl();
const ptAvg = new PositionTracker(POSITION_AVG_COST);
void ptAvg.position(1);

const pgt = new PositionGroupTracker();
const posId = pgt.openPosition(1, 1, "buy", 100, 0.1);
pgt.closePosition(posId, 105);
pgt.partialClose(posId, 0.05, 105);
void pgt.netPosition(1);
void pgt.openCount(1);
pgt.prune();

const ot = new OrderTracker();
void ot.onSubmitted(1, 1, "buy", 100, 0.1);
void ot.onFilled(1, 0.1);
void ot.onCanceled(1);
void ot.isActive(1);
const _ac: number = ot.activeCount;
const _tc: number = ot.totalCount;
ot.prune();

// ── Profiles ──────────────────────────────────────────────────────────
const vp = new VolumeProfile(0.01);
vp.addTrade(100, 0.1, true);
void vp.poc();
void vp.valueAreaHigh();
void vp.valueAreaLow();
void vp.totalVolume();
vp.clear();

let mp!: MarketProfile;
checkAcceptsBigint(() => { mp = new MarketProfile(0.01, 30, 0n); }, "MarketProfile constructor accepts bigint ts");
checkAcceptsBigint(() => mp.addTrade(0n, 100, 0.1, true), "MarketProfile.addTrade accepts bigint ts");
void mp.poc();
void mp.initialBalanceHigh();
void mp.initialBalanceLow();
void mp.isPoorHigh();
void mp.isPoorLow();
mp.clear();

const fb = new FootprintBar(0.01);
fb.addTrade(100, 0.1, true);
void fb.totalDelta();
void fb.totalVolume();
const _nl: number = fb.numLevels;
fb.clear();

// ── Stats ─────────────────────────────────────────────────────────────
const _corr: number = lrvx.correlation(arr, arr);
void _corr;
void lrvx.profitFactor(arr);
void lrvx.winRate(arr);
const ci = lrvx.bootstrapCI(arr, 0.95, 1000);
void ci.lower;
void ci.median;
void ci.upper;
const sigInt = new Int8Array([1, 0, -1, 0, 1]);
void lrvx.permutationTest(arr, arr, 1000);
void lrvx.barReturns(sigInt, sigInt, arr);
void lrvx.tradePnl(sigInt, sigInt, arr);

// ── Data I/O ─────────────────────────────────────────────────────────
// A fresh directory per run: this block writes/reads/re-writes the
// same path in sequence (DataWriter -> DataReader -> BinaryLogRecorderHook
// -> Partitioner), and a fixed path accumulated trades across repeated
// local runs (DataReader.readTrades returning more each time), unlike a
// CI job which always starts from a clean /tmp.
const testDataDir = `/tmp/lrvx-test-types-${Date.now()}`;
const dw = new DataWriter(testDataDir, 64, 1);
void dw.writeTrade(0n, 0n, 100, 0.1, 1, 1, "buy");
dw.flush();
const dwStats = dw.stats();
void dwStats.bytesWritten;
void dwStats.eventsWritten;
dw.close();

const dr = new DataReader(testDataDir, 0n, 1n);
const _drCount: number = dr.count;
const summary = dr.summary();
void summary.totalEvents;
void summary.durationSeconds;
// Clock readings are declared `bigint` and must be one at runtime.
const _summaryFirst: bigint = summary.firstEventNs;
const _summaryLast: bigint = summary.lastEventNs;
check(typeof _summaryFirst === "bigint" && typeof _summaryLast === "bigint",
      "DataReaderSummary.firstEventNs / lastEventNs are bigint at runtime");
const drStats = dr.stats();
void drStats.tradesRead;
const trades: TradeRecord[] = dr.readTrades(100);
void trades[0]?.price;
check(trades.length === 1, `DataReader.readTrades returns the trade written above (got ${trades.length})`);
if (trades[0]) {
  // TradeRecord declares `bigint` -- DataReader and MergedTapeReader
  // must agree, or `a.exchangeTsNs === b.exchangeTsNs` is always false
  // across the two readers.
  check(typeof trades[0].exchangeTsNs === "bigint", "TradeRecord.exchangeTsNs is bigint at runtime");
  // TradeRecord.side is declared "buy" | "sell", not a raw number.
  check(trades[0].side === "buy", `TradeRecord.side is the string "buy" (got ${JSON.stringify(trades[0].side)})`);
}
void dr.readTradesFrom(0n, 100);
const bbo: BboRecord[] = dr.readBBO();
void bbo[0]?.bidPrice;
void dr.readBBOFrom(0n);
const bookU: BookUpdateRecord[] = dr.readBookUpdates();
void bookU[0]?.bids;
void dr.readBookUpdatesFrom(0n);

const recorder = new BinaryLogRecorderHook(testDataDir, 64, 0, "lz4");
recorder.addSymbol(1, "BTCUSDT", "BTC", "USDT", 2, 8);
recorder.flush();
const _recStats = recorder.stats();
void _recStats.tradesWritten;

const part = new Partitioner(testDataDir);
let parts: Partition[] = [];
checkAcceptsBigint(() => { parts = part.byTime(4, 0n); }, "Partitioner.byTime accepts bigint warmup");
void parts[0]?.fromNs;
checkAcceptsBigint(() => part.byDuration(1_000_000n, 0n), "Partitioner.byDuration accepts bigint duration and warmup");
void part.byCalendar('day', 0n);
void part.bySymbol(2);
void part.perSymbol();
void part.byEventCount(4);

// ── Segment ops ──────────────────────────────────────────────────────
const _vsOk: boolean = lrvx.validateSegment("/tmp/x");
void _vsOk;
const valRes = lrvx.validate("/tmp/x");
void valRes.headerValid;
void valRes.tradesFound;
const dsRes = lrvx.validateDataset("/tmp/x");
void dsRes.totalSegments;
void dsRes.firstTimestamp;
const _msOk: boolean = lrvx.mergeSegments("/tmp/a", "/tmp/b");
void _msOk;
const mdRes = lrvx.mergeDir("/tmp/a", "/tmp/b");
void mdRes.segmentsMerged;
const splitRes = lrvx.split("/tmp/a", "/tmp/b", "time", 3_600_000_000_000n, 1_000_000);
void splitRes.segmentsCreated;
const exRes = lrvx.exportData("/tmp/a", "/tmp/b", "json", 0n, 0n);
void exRes.eventsExported;
const _rcOk: boolean = lrvx.recompress("/tmp/a", "/tmp/b", "lz4");
void _rcOk;
const _esCount: number = lrvx.extractSymbols(
  "/tmp/a",
  "/tmp/b",
  new Uint32Array([1, 2, 3]),
);
void _esCount;
const _etCount: number = lrvx.extractTimeRange("/tmp/a", "/tmp/b", 0n, 1n);
void _etCount;
const insp = lrvx.inspect("/tmp/x");
void insp.totalEvents;
void insp.durationSeconds;
void lrvx.rollingCorrelation(arr, arr, 20);

// ── Indicator graphs ──────────────────────────────────────────────────
const ig = new IndicatorGraph();
ig.setBars(1, arr, arr, arr, arr);
ig.addNode("emaClose", [], (g, sym) => {
  const closes = g.close(sym);
  return closes;
});
const _igSeries: Float64Array = ig.require(1, "emaClose");
void _igSeries;
void ig.get(1, "emaClose");
void ig.close(1);
void ig.high(1);
void ig.low(1);
void ig.volume(1);
ig.invalidate(1);
ig.invalidateAll();

const sg = new StreamingIndicatorGraph();
sg.addNode("triple", [], (g, sym) => g.close(sym));
sg.step(1, 100);
sg.step(1, 100, 100, 100, 1);
void sg.current(1, "triple");
void sg.barCount(1);
sg.reset(1);
sg.resetAll();
void sg.close(1);

// ── targets submodule ────────────────────────────────────────────────
const fr: Float64Array = targets.future_return(arr, 5);
void fr;
void targets.future_ctc_volatility(arr, 5);
void targets.future_linear_slope(arr, 5);

// ── Clock inputs accept bigint ───────────────────────────────────────
{
  const T = 1757000000123456789n;
  const va = new lrvx.VenueAvailability();
  checkAcceptsBigint(() => va.scheduleOutage(T, 10), "VenueAvailability.scheduleOutage accepts bigint startNs");
  checkAcceptsBigint(() => va.scheduleOutageEx(T, 10, "total"), "VenueAvailability.scheduleOutageEx accepts bigint startNs");
  checkAcceptsBigint(() => {
    const up: boolean = va.isUp(T);
    void up;
    void va.submitsAllowed(T);
    void va.cancelsAllowed(T);
    void va.bookUpdatesAllowed(T);
    void va.tradesAllowed(T);
    const mult: number = va.latencyMultiplier(T);
    void mult;
  }, "VenueAvailability queries accept bigint nowNs");

  const acct = new lrvx.Account(1, 1000);
  checkAcceptsBigint(() => acct.setMark(1, 100, T), "Account.setMark accepts bigint tsNs");
  checkAcceptsBigint(() => void acct.hasStaleMarks(T, 100), "Account.hasStaleMarks accepts bigint nowNs");
  checkAcceptsBigint(() => acct.recordFill(T, 100, 1), "Account.recordFill accepts bigint tsNs");

  const fs = new lrvx.FundingSchedule();
  checkAcceptsBigint(() => fs.setTape([T, 5], [0.001, 0.002]), "FundingSchedule.setTape accepts bigint timestamps");
  checkAcceptsBigint(() => fs.setTapeBySymbol([T], [1], [0.001]), "FundingSchedule.setTapeBySymbol accepts bigint timestamps");
  checkAcceptsBigint(() => void fs.tick(T, [1], [1], [100]), "FundingSchedule.tick accepts bigint nowNs");

  const group = new lrvx.OrderGroup();
  checkAcceptsBigint(() => void group.pairLatencyDecision({ leaderSubmitTsNs: T, leaderAckTsNs: T + 5n, ackReceived: true }),
                     "OrderGroup.pairLatencyDecision accepts bigint timestamps");

  const liq = new lrvx.LiquidationEngine();
  checkAcceptsBigint(() => void liq.onMarks([[1, 100]], T), "LiquidationEngine.onMarks accepts bigint tsNs");

  const stack = lrvx.VenueStack.binanceUmFutures(1, 1000);
  checkAcceptsBigint(() => stack.feesRecordFill(T, 100), "VenueStack.feesRecordFill accepts bigint tsNs");
  checkAcceptsBigint(() => void stack.feesFor(T, 100, true), "VenueStack.feesFor accepts bigint tsNs");

  const tape = lrvx.poolTapeCreate();
  lrvx.poolTapeDescriptorConstantProduct(tape, 3, 1000, 18, 6);
  checkAcceptsBigint(() => lrvx.poolTapeCheckpoint(tape, T, 1000000n, 1000000n), "poolTapeCheckpoint accepts bigint tsNs");
  checkAcceptsBigint(() => lrvx.poolTapeSwap(tape, T + 1n, true, 1000n), "poolTapeSwap accepts bigint tsNs");

  const recorder = new lrvx.BarDispatchRecorder();
  recorder.addTimeIntervalSeconds(1);
  checkAcceptsBigint(() => recorder.onTrade(1, 100, 1, T), "BarDispatchRecorder.onTrade accepts bigint tsNs");
}

// ── Constants exist ──────────────────────────────────────────────────
void SLIPPAGE_NONE;
void SLIPPAGE_FIXED_BPS;
void SLIPPAGE_FIXED_TICKS;
void SLIPPAGE_VOLUME_IMPACT;
void QUEUE_NONE;
void QUEUE_TOB;
void QUEUE_FULL;
void POSITION_FIFO;
void POSITION_AVG_COST;

console.log(`test_types: ${_passed} passed, ${_failed} failed`);
process.exitCode = _failed > 0 ? 1 : 0;

// No forced exit. The threaded Runner above releases its channel on stop(),
// so the loop drains and the process ends on its own; if it ever stops doing
// that, this file has to say so rather than paper over it. An unref'd timer
// does not keep the loop alive by itself, so it fires only when something
// else is holding the loop open -- turning what once cost a CI run hours
// into a ten-second failure with its own exit code.
const _loopWatchdog = setTimeout(() => {
  console.error("test_types: the event loop is still alive 10s after the last check");
  process.exit(3);
}, 10000);
_loopWatchdog.unref();
