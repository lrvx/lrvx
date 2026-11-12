'use strict';
/**
 * node/test/test_stats_trade_times.js -- every stats object carries the
 * trade durations, the time-weighted return and the time bounds of the run:
 * BacktestResult.stats(), Engine.run(), walk-forward fold stats and grid
 * search results, the same fields BacktestRunner.run* reports.
 *
 * Run from repo root:
 *   cd node && node test/test_stats_trade_times.js
 */

const fs = require('fs');
const os = require('os');
const path = require('path');
const lrvx = require(path.join(__dirname, '..'));

let passed = 0;
let failed = 0;
function check(cond, msg) {
  if (cond) { passed++; console.log(`  ok  ${msg}`); }
  else { failed++; console.error(`  FAIL  ${msg}`); }
}

const SEC = 1000000000n;
// Not a multiple of the 256 ns step a double moves in at this magnitude.
const T0 = 1767225600n * SEC + 789n;

function checkRoundTrip(label, s, start, end, durationNs) {
  check(s.avgTradeDurationNs === durationNs,
        `${label}: avgTradeDurationNs (${s.avgTradeDurationNs})`);
  check(s.medianTradeDurationNs === durationNs,
        `${label}: medianTradeDurationNs (${s.medianTradeDurationNs})`);
  check(s.maxTradeDurationNs === durationNs,
        `${label}: maxTradeDurationNs (${s.maxTradeDurationNs})`);
  check(typeof s.timeWeightedReturn === 'number' && s.timeWeightedReturn > 0,
        `${label}: timeWeightedReturn (${s.timeWeightedReturn})`);
  check(typeof s.startTimeNs === 'bigint' && s.startTimeNs === start,
        `${label}: startTimeNs is the first fill, to the nanosecond (${s.startTimeNs})`);
  check(typeof s.endTimeNs === 'bigint' && s.endTimeNs === end,
        `${label}: endTimeNs is the last fill, to the nanosecond (${s.endTimeNs})`);
}

console.log('=== BacktestResult.stats ===');
{
  const result = new lrvx.BacktestResult(10000, 0);
  result.recordFill(1, 1, 'buy', 100, 1, T0);
  result.recordFill(2, 1, 'sell', 110, 1, T0 + 9n * SEC);
  const s = result.stats();
  check(s.totalTrades === 1, `one closed trade (${s.totalTrades})`);
  checkRoundTrip('BacktestResult', s, T0, T0 + 9n * SEC, 9e9);

  const empty = new lrvx.BacktestResult(10000, 0).stats();
  check(empty.avgTradeDurationNs === 0 && empty.startTimeNs === 0n && empty.endTimeNs === 0n,
        'a result with no fills reports zero durations and 0n bounds');
}

// 400 one-minute bars: a slow sine around 100, so an SMA cross trades.
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'lrvx-stats-times-'));
const csv = path.join(root, 'bars.csv');
const BARS = 400;
const CSV_T0_MS = 1767225600000;
{
  const rows = ['timestamp,open,high,low,close,volume'];
  for (let i = 0; i < BARS; i++) {
    const px = (100 + 5 * Math.sin(i / 8)).toFixed(4);
    rows.push(`${CSV_T0_MS + i * 60000},${px},${px},${px},${px},1`);
  }
  fs.writeFileSync(csv, rows.join('\n') + '\n');
}
const CSV_FIRST_NS = BigInt(CSV_T0_MS) * 1000000n;
const CSV_LAST_NS = BigInt(CSV_T0_MS + (BARS - 1) * 60000) * 1000000n;

function checkPlausible(label, s) {
  check(s.totalTrades > 0, `${label}: produced trades (${s.totalTrades})`);
  check(typeof s.avgTradeDurationNs === 'number' && s.avgTradeDurationNs >= 60e9,
        `${label}: avgTradeDurationNs spans at least one bar (${s.avgTradeDurationNs})`);
  check(typeof s.medianTradeDurationNs === 'number' && s.medianTradeDurationNs > 0 &&
        s.maxTradeDurationNs >= s.medianTradeDurationNs,
        `${label}: median and max durations (${s.medianTradeDurationNs}, ${s.maxTradeDurationNs})`);
  check(typeof s.timeWeightedReturn === 'number' && Number.isFinite(s.timeWeightedReturn),
        `${label}: timeWeightedReturn (${s.timeWeightedReturn})`);
  check(typeof s.startTimeNs === 'bigint' && s.startTimeNs >= CSV_FIRST_NS &&
        s.startTimeNs <= CSV_LAST_NS,
        `${label}: startTimeNs is a bigint inside the data (${s.startTimeNs})`);
  check(typeof s.endTimeNs === 'bigint' && s.endTimeNs > s.startTimeNs &&
        s.endTimeNs <= CSV_LAST_NS,
        `${label}: endTimeNs is a bigint after startTimeNs (${s.endTimeNs})`);
}

console.log('=== walk-forward fold stats ===');
{
  const reg = new lrvx.SymbolRegistry();
  const sym = reg.addSymbol('test', 'AAA', 0.0001);
  const wfr = new lrvx.WalkForwardRunner(reg, 0, 10000, {
    mode: 'anchored', testSize: 100, step: 100, minTrainSize: 100,
  });
  wfr.setStrategyFactory(() => {
    const fast = new lrvx.SMA(3);
    const slow = new lrvx.SMA(10);
    return {
      symbols: [Number(sym)],
      onTrade(ctx, t, emit) {
        const f = fast.update(t.price);
        const s = slow.update(t.price);
        if (f === null || s === null || !slow.ready) { return; }
        if (f > s && ctx.position === 0) { emit.marketBuy(1); }
        else if (f < s && ctx.position > 0) { emit.marketSell(1); }
      },
    };
  });
  const folds = wfr.runCsv(csv, 'AAA');
  check(folds.length === 3, `three folds (${folds.length})`);
  const fold = folds[folds.length - 1] || {trainStats: {}, testStats: {}};
  checkPlausible('fold.trainStats', fold.trainStats);
  checkPlausible('fold.testStats', fold.testStats);
}

console.log('=== Engine.run ===');
{
  const engine = new lrvx.Engine(10000, 0);
  engine.loadCsv(csv, 'AAA');
  const ts = engine.ts('AAA');
  const signals = new lrvx.SignalBuilder();
  signals.buy(ts[10], 1, 'AAA');
  signals.sell(ts[30], 1, 'AAA');
  const s = engine.run(signals);
  check(s.totalTrades === 1, `one closed trade (${s.totalTrades})`);
  check(s.avgTradeDurationNs === 20 * 60e9, `avgTradeDurationNs (${s.avgTradeDurationNs})`);
  check(s.maxTradeDurationNs === 20 * 60e9, `maxTradeDurationNs (${s.maxTradeDurationNs})`);
  check(typeof s.timeWeightedReturn === 'number', `timeWeightedReturn (${s.timeWeightedReturn})`);
  check(typeof s.startTimeNs === 'bigint' && s.startTimeNs === ts[10],
        `startTimeNs is the first fill (${s.startTimeNs})`);
  check(typeof s.endTimeNs === 'bigint' && s.endTimeNs === ts[30],
        `endTimeNs is the last fill (${s.endTimeNs})`);
}

console.log('=== grid search results keep what the factory returned ===');
{
  const reg = new lrvx.SymbolRegistry();
  const sym = reg.addSymbol('test', 'AAA', 0.0001);
  const gs = new lrvx.GridSearch();
  gs.addAxis([1]);
  let fromRunner = null;
  gs.setFactory(() => {
    const bt = new lrvx.BacktestRunner(reg, 0, 10000);
    let seen = 0;
    bt.setStrategy({
      symbols: [sym],
      onTrade(_ctx, _t, emit) {
        seen += 1;
        if (seen === 1) { emit.marketBuy(1); }
        else if (seen === 10) { emit.marketSell(1); }
      },
    });
    fromRunner = bt.runCsv(csv, 'AAA');
    return fromRunner;
  });
  const results = gs.run();
  check(results.length === 1 && fromRunner !== null, 'one grid point ran');
  const s = (results[0] || {stats: {}}).stats;
  const want = fromRunner || {};
  check(want.avgTradeDurationNs > 0 && want.startTimeNs > 0n, 'the runner reported its trade times');
  for (const key of ['avgTradeDurationNs', 'medianTradeDurationNs', 'maxTradeDurationNs',
                     'timeWeightedReturn', 'startTimeNs', 'endTimeNs']) {
    check(typeof s[key] === typeof want[key] && s[key] === want[key],
          `grid stats ${key} matches the runner's (${s[key]} vs ${want[key]})`);
  }
}

fs.rmSync(root, {recursive: true, force: true});

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed > 0 ? 1 : 0);
