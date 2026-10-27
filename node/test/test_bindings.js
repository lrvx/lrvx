'use strict';
/**
 * node/test/test_bindings.js — smoke-test all major Node.js binding surfaces.
 *
 * Run from repo root:
 *   cd node && node test/test_bindings.js
 */

const path = require('path');
const fs   = require('fs');
const os   = require('os');
const lrvx = require(path.join(__dirname, '..'));

let passed = 0;
let failed = 0;

function check(condition, msg) {
  if (condition) {
    console.log(`  ok  ${msg}`);
    passed++;
  } else {
    console.error(`  FAIL  ${msg}`);
    failed++;
  }
}

function approx(a, b, eps = 1e-6) {
  return isFinite(a) && isFinite(b) && Math.abs(a - b) < eps;
}

// ── Constants ─────────────────────────────────────────────────────────

console.log('=== Constants ===');
check(lrvx.POSITION_FIFO === 0, 'POSITION_FIFO === 0');
check(lrvx.POSITION_AVG_COST === 1, 'POSITION_AVG_COST === 1');
check(lrvx.SLIPPAGE_NONE === 0, 'SLIPPAGE_NONE === 0');
check(lrvx.SLIPPAGE_FIXED_TICKS === 1, 'SLIPPAGE_FIXED_TICKS === 1');
check(lrvx.SLIPPAGE_FIXED_BPS === 2, 'SLIPPAGE_FIXED_BPS === 2');
check(lrvx.SLIPPAGE_VOLUME_IMPACT === 3, 'SLIPPAGE_VOLUME_IMPACT === 3');
check(lrvx.QUEUE_NONE === 0, 'QUEUE_NONE === 0');
check(lrvx.QUEUE_TOB === 1, 'QUEUE_TOB === 1');
check(lrvx.QUEUE_FULL === 2, 'QUEUE_FULL === 2');

// ── Streaming indicators ──────────────────────────────────────────────

console.log('=== Streaming indicators ===');

const sma = new lrvx.SMA(3);
sma.update(1.0); sma.update(2.0); sma.update(3.0);
check(approx(sma.value, 2.0), 'SMA(3) of [1,2,3] == 2.0');
sma.update(4.0);
check(approx(sma.value, 3.0), 'SMA(3) shifts to [2,3,4] == 3.0');
check(sma.ready === true, 'SMA.ready === true');

const ema = new lrvx.EMA(3);
for (let i = 1; i <= 10; i++) ema.update(i);
check(ema.value > 0, 'EMA.value > 0 after 10 updates');
check(ema.ready === true, 'EMA.ready === true');

const rsi = new lrvx.RSI(14);
for (let i = 1; i <= 20; i++) rsi.update(i);
check(rsi.value >= 0 && rsi.value <= 100, `RSI in [0,100], got ${rsi.value.toFixed(2)}`);

const atr = new lrvx.ATR(14);
for (let i = 0; i < 20; i++) atr.update(100 + i, 99 + i, 100 + i);
check(atr.value > 0, 'ATR > 0');

// ── Batch indicators ──────────────────────────────────────────────────

console.log('=== Batch indicators ===');

const prices = new Float64Array([1, 2, 3, 4, 5, 6, 7]);
const bSma = lrvx.sma(prices, 3);
check(approx(bSma[2], 2.0), 'batch sma[2] == 2.0');
check(approx(bSma[6], 6.0), 'batch sma[6] == 6.0');

const bEma = lrvx.ema(prices, 3);
check(bEma.length === prices.length, 'batch ema length matches input');
check(bEma[6] > bEma[2], 'batch ema increases for ascending input');

// ── Targets (forward-looking labels) ──────────────────────────────────

console.log('=== Targets ===');

const tClose = new Float64Array([100.0, 101.0, 99.0, 105.0, 110.0]);
const fr = lrvx.targets.future_return(tClose, 2);
check(approx(fr[0], 99.0 / 100.0 - 1.0), 'targets.future_return[0]');
check(approx(fr[2], 110.0 / 99.0 - 1.0), 'targets.future_return[2]');
check(Number.isNaN(fr[3]) && Number.isNaN(fr[4]), 'targets.future_return tail NaN');

const constClose = new Float64Array(20).fill(100.0);
const vol = lrvx.targets.future_ctc_volatility(constClose, 5);
check(approx(vol[0], 0.0), 'targets.future_ctc_volatility const-series == 0');
check(Number.isNaN(vol[19]), 'targets.future_ctc_volatility tail NaN');

const linear = new Float64Array(20);
for (let i = 0; i < 20; ++i) linear[i] = 100.0 + 0.5 * i;
const sl = lrvx.targets.future_linear_slope(linear, 4);
check(approx(sl[0], 0.5), 'targets.future_linear_slope linear-series == 0.5');
check(Number.isNaN(sl[19]), 'targets.future_linear_slope tail NaN');

// ── ADF stationarity test ─────────────────────────────────────────────

console.log('=== ADF ===');

function gaussian(seed) {
  // simple deterministic Box-Muller
  let s = seed;
  return function() {
    s = (s * 1103515245 + 12345) & 0x7fffffff;
    const u1 = ((s + 1) / 0x80000000);
    s = (s * 1103515245 + 12345) & 0x7fffffff;
    const u2 = ((s + 1) / 0x80000000);
    return Math.sqrt(-2 * Math.log(u1)) * Math.cos(2 * Math.PI * u2);
  };
}

const N = 500;
const gen = gaussian(42);
const walk = new Float64Array(N);
walk[0] = 0;
for (let i = 1; i < N; ++i) walk[i] = walk[i - 1] + gen();

const r = lrvx.adf(walk, 4, 'c');
check(r.usedLag <= 4, 'adf usedLag <= max_lag');
check(r.pValue > 0.05, 'adf does not reject H0 for random walk');

const gen2 = gaussian(7);
const noise = new Float64Array(N);
for (let i = 0; i < N; ++i) noise[i] = gen2() * 0.5;
const r2 = lrvx.adf(noise, 4, 'c');
check(r2.testStat < r.testStat, 'stationary series produces lower testStat');
check(r2.pValue < 0.05, 'adf rejects H0 for white noise');
// ── AutoCorrelation ───────────────────────────────────────────────────

console.log('=== AutoCorrelation ===');

const acLinear = new Float64Array(50);
for (let i = 0; i < 50; ++i) acLinear[i] = 5.0 + 0.7 * i;
const ac = lrvx.autocorrelation(acLinear, 10, 1);
check(Number.isNaN(ac[9]), 'autocorrelation warmup is NaN');
check(approx(ac[10], 1.0), 'autocorrelation linear lag=1 == 1.0');

const acStream = new lrvx.AutoCorrelation(10, 1);
let lastStreamed = undefined;
for (let i = 0; i < acLinear.length; ++i) {
  lastStreamed = acStream.update(acLinear[i]);
}
check(approx(lastStreamed, ac[ac.length - 1]),
      'streaming AutoCorrelation matches batch on last index');

// ── IndicatorGraph (batch) ────────────────────────────────────────────

console.log('=== IndicatorGraph (batch) ===');

const ramp = new Float64Array(50);
for (let i = 0; i < 50; ++i) ramp[i] = i;

const g = new lrvx.IndicatorGraph();
g.setBars(0, ramp);

g.addNode('ema5', [], (graph, sym) => lrvx.ema(graph.close(sym), 5));
g.addNode('sma5', [], (graph, sym) => lrvx.sma(graph.close(sym), 5));
g.addNode('diff', ['ema5', 'sma5'], (graph, sym) => {
  const a = graph.get(sym, 'ema5');
  const b = graph.get(sym, 'sma5');
  const out = new Float64Array(a.length);
  for (let i = 0; i < a.length; ++i) out[i] = a[i] - b[i];
  return out;
});

const ema5 = g.require(0, 'ema5');
const diff = g.require(0, 'diff');
check(ema5.length === 50, 'graph require returns full-length array');
check(diff.length === 50, 'graph dependent node has same length');
check(g.get(0, 'sma5') !== null, 'graph get returns cached node array');
check(g.get(0, 'never_added') === null, 'graph get on missing node returns null');

let threw = false;
try { g.require(0, 'missing'); } catch (e) { threw = true; }
check(threw, 'graph require on unknown node throws');

// ── StreamingIndicatorGraph ────────────────────────────────────────────

console.log('=== StreamingIndicatorGraph ===');

const sg = new lrvx.StreamingIndicatorGraph();
sg.addNode('double_close', [], (graph, sym) => {
  const c = graph.close(sym);
  const out = new Float64Array(c.length);
  for (let i = 0; i < c.length; ++i) out[i] = c[i] * 2.0;
  return out;
});

const closesS = [10.0, 20.0, 30.0, 40.0, 50.0];
for (const c of closesS) sg.step(0, c);

check(Math.abs(sg.current(0, 'double_close') - 100.0) < 1e-9, 'streaming current after 5 steps');
check(sg.barCount(0) === 5, 'streaming barCount == 5');

// Parity check.
const bg2 = new lrvx.IndicatorGraph();
bg2.setBars(0, new Float64Array(closesS));
bg2.addNode('double_close', [], (graph, sym) => {
  const c = graph.close(sym);
  const out = new Float64Array(c.length);
  for (let i = 0; i < c.length; ++i) out[i] = c[i] * 2.0;
  return out;
});
const batchDc = bg2.require(0, 'double_close');
check(Math.abs(sg.current(0, 'double_close') - batchDc[batchDc.length - 1]) < 1e-9,
      'streaming current == batch last element');

sg.reset(0);
check(sg.barCount(0) === 0, 'after reset barCount == 0');
check(Number.isNaN(sg.current(0, 'double_close')), 'after reset current is NaN');

// ── PositionTracker ───────────────────────────────────────────────────

console.log('=== PositionTracker ===');

const pt = new lrvx.PositionTracker(lrvx.POSITION_FIFO);
pt.onFill(0, 'buy', 50000.0, 1.0);
check(approx(pt.position(0), 1.0), 'position == 1.0 after buy');
check(approx(pt.avgEntryPrice(0), 50000.0), 'avgEntryPrice == 50000');
check(approx(pt.realizedPnl(0), 0.0), 'realizedPnl == 0 before close');

pt.onFill(0, 'sell', 51000.0, 1.0);
check(approx(pt.position(0), 0.0), 'position == 0 after sell');
check(pt.realizedPnl(0) > 0, 'realizedPnl > 0 on winning trade');
check(approx(pt.realizedPnl(0), 1000.0), 'realizedPnl == 1000');
check(approx(pt.totalRealizedPnl(), 1000.0), 'totalRealizedPnl == 1000');

// Multi-symbol
const pt2 = new lrvx.PositionTracker(lrvx.POSITION_FIFO);
pt2.onFill(0, 'buy', 100.0, 2.0);
pt2.onFill(1, 'sell', 200.0, 3.0);
check(approx(pt2.position(0), 2.0), 'symbol 0 position == 2.0');
check(approx(pt2.position(1), -3.0), 'symbol 1 position == -3.0');

// ── OrderTracker ──────────────────────────────────────────────────────

console.log('=== OrderTracker ===');

const ot = new lrvx.OrderTracker();
check(ot.onSubmitted(1001, 0, 'buy', 50000.0, 1.0) === true, 'onSubmitted returns true');
check(ot.isActive(1001) === true, 'order 1001 is active');
check(ot.activeCount === 1, 'activeCount === 1');
check(ot.totalCount === 1, 'totalCount === 1');

ot.onSubmitted(1002, 0, 'sell', 51000.0, 1.0);
check(ot.activeCount === 2, 'activeCount === 2 after second order');

check(ot.onFilled(1001, 1.0) === true, 'onFilled returns true');
check(ot.isActive(1001) === false, 'order 1001 inactive after fill');
check(ot.activeCount === 1, 'activeCount === 1 after fill');

check(ot.onCanceled(1002) === true, 'onCanceled returns true');
check(ot.isActive(1002) === false, 'order 1002 inactive after cancel');
check(ot.activeCount === 0, 'activeCount === 0');
check(ot.totalCount === 2, 'totalCount === 2');

// ── Engine (data loading) ─────────────────────────────────────────────

console.log('=== Engine ===');

const csvPath = path.join(__dirname, '..', '..', 'docs', 'examples', 'data', 'btcusdt_1m.csv');
if (fs.existsSync(csvPath)) {
  const engine = new lrvx.Engine();
  engine.loadCsv(csvPath);
  const n = engine.barCount();
  check(n > 1000, `barCount ${n} > 1000`);
  const opens  = engine.open();
  const closes = engine.close();
  const highs  = engine.high();
  const lows   = engine.low();
  check(opens.length  === n, 'opens.length  == barCount');
  check(closes.length === n, 'closes.length == barCount');
  check(opens[0] > 0, 'first open price > 0');
  check(highs.every((h, i) => h >= lows[i]), 'high >= low for every bar');
  console.log(`  loaded ${n} bars, price ${Math.min(...closes).toFixed(0)}–${Math.max(...closes).toFixed(0)}`);
} else {
  console.log(`  skip Engine (CSV not found: ${csvPath})`);
}

{
  // Engine.ts() returns a BigInt64Array. parseCsv reads the column from
  // text into an int64, so this reading is exact in the store; handing it
  // back through a Float64Array rounded it to 1765615835519000064.
  const NS = 1765615835519000000n;
  const tmp = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'lrvx-engine-ts-')), 'ns.csv');
  fs.writeFileSync(tmp, `ts,open,high,low,close,volume\n${NS},100,101,99,100.5,10\n`);
  const engine = new lrvx.Engine();
  engine.loadCsv(tmp, 'NS');
  const ts = engine.ts('NS');
  check(ts instanceof BigInt64Array, `Engine.ts() is a BigInt64Array (got ${ts.constructor.name})`);
  check(ts[0] === NS, `a reading past 2^53 survives Engine.ts() (got ${ts[0]}, want ${NS})`);

  // The read side is the write side's argument: SignalBuilder has to take
  // what Engine.ts() hands out.
  const sigs = new lrvx.SignalBuilder();
  let sigErr = null;
  try { sigs.buy(ts[0], 1.0, 'NS'); } catch (e) { sigErr = e; }
  check(sigErr === null,
        `SignalBuilder.buy takes an Engine.ts() element` + (sigErr ? ` (threw ${sigErr.message})` : ''));

  fs.rmSync(path.dirname(tmp), { recursive: true, force: true });
}

// ── BacktestRunner accessors ──────────────────────────────────────────

console.log('=== BacktestRunner equityCurve / trades ===');

const btCsv = path.join(__dirname, '..', '..',
                        'python', 'lrvx', 'templates', 'research',
                        'data', 'btcusdt_sample.csv');
if (fs.existsSync(btCsv)) {
  const reg2 = new lrvx.SymbolRegistry();
  const btc2 = reg2.addSymbol('exchange', 'BTCUSDT', 0.01);

  const fast = new lrvx.SMA(10);
  const slow = new lrvx.SMA(30);

  let lpFnsPresent = false;
  const strat = {
    symbols: [Number(btc2)],
    onTrade(ctx, t, emit) {
      // The emit object exposes the DEX liquidity helpers.
      if (typeof emit.provideLiquidity === 'function' &&
          typeof emit.withdrawLiquidity === 'function') {
        lpFnsPresent = true;
      }
      const f = fast.update(t.price);
      const s = slow.update(t.price);
      if (f === null || s === null) return;
      // Long-only crossover with a real exit side. `totalTrades`
      // counts round-trip trades, so the strategy needs both
      // entry (flat → long) and exit (long → flat) to surface.
      if (f > s && ctx.position === 0) emit.marketBuy(0.01);
      else if (f < s && ctx.position > 0) emit.marketSell(0.01);
    },
  };

  const bt = new lrvx.BacktestRunner(reg2, 0.0004, 10000);
  bt.setStrategy(strat);
  const stats = bt.runCsv(btCsv, 'BTCUSDT');
  check(stats !== null && stats.totalTrades > 0,
        `BacktestRunner produced trades (got ${stats && stats.totalTrades})`);
  check(lpFnsPresent,
        'emit exposes provideLiquidity / withdrawLiquidity');

  const eq = bt.equityCurve();
  check(eq && eq.equity instanceof Float64Array,
        'equityCurve.equity is Float64Array');
  check(eq.timestampNs instanceof BigInt64Array,
        'equityCurve.timestampNs is BigInt64Array');
  check(eq.equity.length === stats.totalTrades,
        `equity length === totalTrades (${eq.equity.length} vs ${stats.totalTrades})`);

  const tr = bt.trades();
  check(tr && tr.pnl instanceof Float64Array,
        'trades.pnl is Float64Array');
  check(tr.symbol instanceof Uint32Array && tr.side instanceof Uint8Array,
        'trades.symbol is Uint32Array, side is Uint8Array');
  check(tr.pnl.length === stats.totalTrades,
        `trades length === totalTrades (${tr.pnl.length} vs ${stats.totalTrades})`);

  const fresh = new lrvx.BacktestRunner(reg2, 0.0004, 10000);
  let threwFresh = false;
  try {
    fresh.equityCurve();
  } catch (e) {
    threwFresh = (e && e.code === 'E_RUN_002');
  }
  check(threwFresh, 'equityCurve before run throws LrvxError E_RUN_002');
} else {
  console.log(`  skip BacktestRunner accessors (CSV not found: ${btCsv})`);
}

// ── BacktestRunner.runTape (parity proof) ────────────────────────────

console.log('=== BacktestRunner.runTape ===');

{
  const reg = new lrvx.SymbolRegistry();
  reg.addSymbol('exchange', 'BTCUSDT', 0.01);
  const bt = new lrvx.BacktestRunner(reg, 0.0004, 10000);
  bt.setStrategy({
    symbols: [],
    onTrade(_ctx, _t, _emit) {},
  });
  check(typeof bt.runTape === 'function',
        'BacktestRunner.runTape is a function');

  // Missing-path case: per the existing runCsv pattern, the C ABI
  // returns null when the underlying call fails. (Some fail modes
  // raise LrvxError instead — accept either.)
  let result = 'pending';
  try { result = bt.runTape('/__nonexistent_tape_path__'); }
  catch (_) { result = 'threw'; }
  check(result === null || result === 'threw',
        `runTape signals failure on missing tape (got ${JSON.stringify(result)})`);
}

// ── Strategy on_fill / on_order_update hooks ─────────────────────────

console.log('=== Strategy onFill / onOrderUpdate ===');

if (fs.existsSync(btCsv)) {
  const reg4 = new lrvx.SymbolRegistry();
  const btc4 = reg4.addSymbol('exchange', 'BTCUSDT', 0.01);
  const fast = new lrvx.SMA(10);
  const slow = new lrvx.SMA(30);

  const fills = [];
  const updates = [];
  const strat = {
    symbols: [Number(btc4)],
    onTrade(ctx, t, emit) {
      const f = fast.update(t.price);
      const s = slow.update(t.price);
      if (f === null || s === null) return;
      if (f > s && ctx.position === 0) emit.marketBuy(0.01);
      else if (f < s && ctx.position > 0) emit.marketSell(0.01);
    },
    onFill(ctx, ev, _emit) { fills.push(ev); },
    onOrderUpdate(ctx, ev, _emit) { updates.push(ev); },
  };

  const bt = new lrvx.BacktestRunner(reg4, 0.0004, 10000);
  bt.setStrategy(strat);
  const stats = bt.runCsv(btCsv, 'BTCUSDT');
  check(stats !== null && stats.totalTrades > 0,
        `onFill backtest produced trades (got ${stats && stats.totalTrades})`);
  check(fills.length > 0, `onFill called at least once (got ${fills.length})`);
  check(fills.every(f =>
          typeof f.orderId === 'number' &&
          (f.side === 'buy' || f.side === 'sell') &&
          (f.status === 'FILLED' || f.status === 'PARTIALLY_FILLED') &&
          f.fillQty > 0 && f.fillPrice > 0),
        'onFill payloads have correct shape');
  check(updates.length >= fills.length,
        `onOrderUpdate fires on every status change (got ${updates.length}; >= fills ${fills.length})`);
} else {
  console.log(`  skip onFill backtest (CSV not found: ${btCsv})`);
}

// ── WalkForwardRunner ─────────────────────────────────────────────────

console.log('=== WalkForwardRunner ===');

const wfCsv = path.join(__dirname, '..', '..',
                        'python', 'lrvx', 'templates', 'research',
                        'data', 'btcusdt_sample.csv');
if (fs.existsSync(wfCsv)) {
  const reg3 = new lrvx.SymbolRegistry();
  const btc3 = reg3.addSymbol('exchange', 'BTCUSDT', 0.01);
  const wfr = new lrvx.WalkForwardRunner(reg3, 0.0004, 10000, {
    mode: 'anchored', testSize: 100, step: 100, minTrainSize: 100,
  });
  wfr.setStrategyFactory(() => {
    const fast = new lrvx.SMA(10);
    const slow = new lrvx.SMA(30);
    return {
      symbols: [Number(btc3)],
      onTrade(ctx, t, emit) {
        const f = fast.update(t.price);
        const s = slow.update(t.price);
        if (f === null || s === null || !slow.ready) return;
        if (f > s && ctx.position === 0) emit.marketBuy(0.01);
        else if (f < s && ctx.position > 0) emit.marketSell(0.01);
      },
    };
  });
  const folds = wfr.runCsv(wfCsv, 'BTCUSDT');
  check(folds.length === 4, `walk-forward fold count == 4 (got ${folds.length})`);
  check(folds.every(f => f.trainStartBar === 0),
        'anchored: every fold trains from bar 0');
  check(folds[0].trainEndBar === 100,
        `first fold train end == 100 (got ${folds[0].trainEndBar})`);
  check(typeof folds[0].trainStartNs === 'bigint',
        'trainStartNs is bigint');
  check(folds[0].trainStats.totalTrades > 0,
        `fold 0 train produced trades (got ${folds[0].trainStats.totalTrades})`);

  const wfr2 = new lrvx.WalkForwardRunner(reg3, 0.0004, 10000, {
    mode: 'sliding', trainSize: 200, testSize: 100, step: 100,
  });
  wfr2.setStrategyFactory(() => {
    const fast = new lrvx.SMA(10), slow = new lrvx.SMA(30);
    return {
      symbols: [Number(btc3)],
      onTrade(ctx, t, emit) {
        const f = fast.update(t.price), s = slow.update(t.price);
        if (f === null || s === null || !slow.ready) return;
        if (f > s && ctx.position === 0) emit.marketBuy(0.01);
        else if (f < s && ctx.position > 0) emit.marketSell(0.01);
      },
    };
  });
  const sFolds = wfr2.runCsv(wfCsv, 'BTCUSDT');
  check(sFolds.length === 3, `sliding fold count == 3 (got ${sFolds.length})`);
  check(sFolds.every(f => f.trainEndBar - f.trainStartBar === 200),
        'sliding: train window is constant 200 bars');
} else {
  console.log(`  skip WalkForwardRunner (CSV not found: ${wfCsv})`);
}

// ── GridSearch ────────────────────────────────────────────────────────

console.log('=== GridSearch ===');

{
  const gs = new lrvx.GridSearch();
  gs.addAxis([1, 2, 3]);
  gs.addAxis([10, 20]);
  check(gs.total() === 6, `grid total == 6 (got ${gs.total()})`);
  const p0 = gs.paramsForIndex(0);
  const p1 = gs.paramsForIndex(1);
  const p2 = gs.paramsForIndex(2);
  check(p0[0] === 1 && p0[1] === 10, `idx 0 = [1,10] (got ${p0})`);
  check(p1[0] === 1 && p1[1] === 20, `idx 1 = [1,20] (got ${p1})`);
  check(p2[0] === 2 && p2[1] === 10, `idx 2 = [2,10] (got ${p2})`);

  const seen = [];
  gs.setFactory((params) => {
    seen.push(params.slice());
    return { sharpeRatio: params[0] + params[1], returnPct: 0, totalTrades: 0 };
  });
  const results = gs.run();
  check(results.length === 6, `run produced 6 results (got ${results.length})`);
  check(seen.length === 6, `factory called 6 times (got ${seen.length})`);
  check(Math.abs(results[5].stats.sharpeRatio - 23) < 1e-9,
        `last result sharpe == 23 (got ${results[5].stats.sharpeRatio})`);
}

// ── Summary ───────────────────────────────────────────────────────────

// ── Heatmap renderer ──────────────────────────────────────────────────

console.log('=== lrvx.report.heatmapHtml ===');
{
  const z = [[0.5, -0.3, 1.2], [0.8, 1.1, -1.4]];
  const html = lrvx.report.heatmapHtml(z, {
    rowLabels: ['fast=5', 'fast=10'],
    colLabels: ['slow=20', 'slow=30', 'slow=50'],
    title: 'Sweep',
    xAxisName: 'slow period',
    yAxisName: 'fast period',
    metricName: 'Sharpe',
  });
  check(html.length > 500, `heatmap html non-empty (${html.length} bytes)`);
  check(html.includes('<svg'), 'heatmap contains <svg>');
  check(html.includes('Sweep'), 'heatmap contains title');
  check(html.includes('fast=5'), 'heatmap contains row labels');
  check(html.includes('slow=30'), 'heatmap contains col labels');
  check(html.includes('Sharpe'), 'heatmap contains metric name');
  check(!html.includes('<script src='), 'heatmap has no external scripts');

  let threwEmpty = false;
  try { lrvx.report.heatmapHtml([]); } catch (e) { threwEmpty = true; }
  check(threwEmpty, 'empty z raises');

  let threwUneven = false;
  try { lrvx.report.heatmapHtml([[1, 2], [3]]); } catch (e) { threwUneven = true; }
  check(threwUneven, 'uneven rows raise');
}

// ── White's reality check ─────────────────────────────────────────────

console.log("=== lrvx.whitesRealityCheck ===");
{
  // (K, T) returns matrix flat in row-major order — one signal-bearing
  // strategy and the rest noise.
  const K = 5;
  const T = 252;
  const SIGNAL_K = 2;
  const flat = new Float64Array(K * T);
  const g = gaussian(2026);
  for (let k = 0; k < K; ++k) {
    for (let t = 0; t < T; ++t) {
      flat[k * T + t] = g() * 0.01 + (k === SIGNAL_K ? 0.005 : 0.0);
    }
  }

  const out = lrvx.whitesRealityCheck(flat, K, T, 2000, 0.0);
  check(typeof out.p_value === "number", `p_value is a number (${out.p_value})`);
  check(out.p_value >= 0 && out.p_value <= 1,
        `p_value in [0,1] (${out.p_value})`);
  check(out.best_index === SIGNAL_K,
        `best_index picks signal strategy (got ${out.best_index})`);
  check(out.best_stat > 0, `best_stat > 0 (${out.best_stat})`);
  check(out.p_value < 0.10,
        `signal strategy is detected (p=${out.p_value})`);

  // Pure noise: every strategy is N(0, 0.01). With K=5 and T=252,
  // the best of 5 noise means is roughly 1.2σ ≈ 0.00075, which can
  // *occasionally* trip a p < 0.05 even though there is no signal.
  // Average across multiple seeds so the assertion is stable.
  let pSum = 0;
  let pRuns = 0;
  for (const noiseSeed of [2027, 2031, 2039, 2053, 2063]) {
    const noise = new Float64Array(K * T);
    const ng = gaussian(noiseSeed);
    for (let i = 0; i < noise.length; ++i) noise[i] = ng() * 0.01;
    const noiseOut = lrvx.whitesRealityCheck(noise, K, T, 1500, 0.0);
    pSum += noiseOut.p_value;
    pRuns++;
  }
  const meanNoiseP = pSum / pRuns;
  check(meanNoiseP > 0.20,
        `pure noise mean p-value > 0.20 across 5 seeds (got ${meanNoiseP.toFixed(3)})`);

  // Determinism — the C ABI uses a fixed seed (42 on the C++ side).
  const a = lrvx.whitesRealityCheck(flat, K, T, 1000, 5.0);
  const b = lrvx.whitesRealityCheck(flat, K, T, 1000, 5.0);
  check(a.p_value === b.p_value,
        `same inputs are deterministic (${a.p_value} vs ${b.p_value})`);
}

console.log("=== lrvx.pboCscv / deflatedSharpe / gridNeighborhood ===");
{
  // Two trials, four periods, two blocks: trial A beats B on every block,
  // so the in-sample best is A and A is also the better one out of sample.
  const consistent = new Float64Array([1, 2, 1, 2, -1, -2, -1, -2]);
  const p = lrvx.pboCscv(consistent, 2, 4, 2);
  check(p.splits === 2, `two blocks give two splits (got ${p.splits})`);
  check(p.pbo === 0, `a consistent winner has pbo 0 (got ${p.pbo})`);
  check(Math.abs(p.mean_oos_rank - 2 / 3) < 1e-12, `mean_oos_rank 2/3 (got ${p.mean_oos_rank})`);
  // The winner of each block loses the other: pbo 1.
  const flipping = new Float64Array([1, 2, -1, -2, -1, -2, 1, 2]);
  check(lrvx.pboCscv(flipping, 2, 4, 2).pbo === 1, "a flipping winner has pbo 1");
  check(Number.isNaN(lrvx.pboCscv(flipping, 2, 4, 3).pbo), "an odd block count is NaN");

  // Deflated Sharpe: with no trials the expected maximum is 0 and dsr is the
  // probabilistic Sharpe ratio; with many trials of spread-out Sharpe ratios
  // the same series is less convincing.
  const g = gaussian(7);
  const r = new Float64Array(400);
  for (let t = 0; t < 400; ++t) r[t] = g() * 0.01 + 0.002;
  const one = lrvx.deflatedSharpe(r);
  check(one.expected_max_sharpe === 0, "no trials: expected maximum is 0");
  check(one.dsr > 0.9 && one.dsr <= 1,
        `a 0.2 sigma edge over 400 periods is significant (dsr=${one.dsr})`);
  check(Math.abs(one.kurtosis - 3) < 1.5, `kurtosis near 3 for gaussian returns (${one.kurtosis})`);
  const trials = new Float64Array(500);
  for (let k = 0; k < 500; ++k) trials[k] = (k - 249.5) / 1000;
  const many = lrvx.deflatedSharpe(r, trials);
  check(many.expected_max_sharpe > 0, "trials give a positive expected maximum");
  check(many.dsr < one.dsr, `more trials, lower dsr (${many.dsr} < ${one.dsr})`);

  // 3x3 grid, radius 1: the centre has 8 neighbours averaging 5, a corner 3.
  const grid = new Float64Array([1, 2, 3, 4, 5, 6, 7, 8, 9]);
  const n = lrvx.gridNeighborhood(grid, [3, 3], 1);
  check(n.neighbor_count[4] === 8 && n.neighbor_mean[4] === 5, "centre: 8 neighbours, mean 5");
  check(n.neighbor_count[0] === 3 && Math.abs(n.neighbor_mean[0] - 11 / 3) < 1e-12,
        "corner: 3 neighbours, mean 11/3");
}

console.log(`\n${passed} passed, ${failed} failed`);
if (failed > 0) process.exit(1);
