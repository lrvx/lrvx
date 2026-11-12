'use strict';
/**
 * node/test/test_backtest_runner_stats.js -- what BacktestRunner.runTape and
 * runTapes hand back: the trade durations, the run's time bounds and the
 * time-weighted return on success, and the reason on failure.
 *
 * Run from repo root:
 *   cd node && node test/test_backtest_runner_stats.js
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

const SEC = 1_000_000_000n;
const T0 = 1767225600n * SEC;
const N = 20;

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'lrvx-runner-stats-'));
const tape = path.join(root, 'tape');

// Recorded through a Runner so the tape carries the manifest runTapes keys
// its symbols by.
{
  const reg = new lrvx.SymbolRegistry();
  const sym = reg.addSymbol('test', 'AAA', 0.01);
  const hook = new lrvx.BinaryLogRecorderHook(tape, 4, 1, 'none', 'test', 'spot');
  hook.addSymbol(Number(sym), 'AAA', '', '', 2, 6);
  const runner = new lrvx.Runner(reg, () => {}, false);
  runner.addStrategy({symbols: [sym]});
  runner.setMarketDataRecorder(hook);
  runner.start();
  for (let i = 0; i < N; i++) {
    runner.onTrade(Number(sym), 100 + i, 1, true, T0 + BigInt(i) * SEC);
  }
  runner.stop();
  hook.flush();
}

// Buys on the first trade and sells on the tenth: one closed trade, nine
// seconds long, on a rising tape.
function roundTripRunner() {
  const reg = new lrvx.SymbolRegistry();
  const sym = reg.addSymbol('test', 'AAA', 0.01);
  const bt = new lrvx.BacktestRunner(reg, 0, 10000);
  let seen = 0;
  bt.setStrategy({
    symbols: [sym],
    onTrade(_ctx, _trade, emit) {
      seen += 1;
      if (seen === 1) { emit.marketBuy(1); }
      else if (seen === 10) { emit.marketSell(1); }
    },
  });
  return bt;
}

for (const [label, run] of [
  ['runTape', (bt) => bt.runTape(tape)],
  ['runTapes', (bt) => bt.runTapes([tape])],
]) {
  console.log(`=== ${label}: stats ===`);
  const stats = run(roundTripRunner());
  check(stats !== null && stats.totalTrades === 1, `${label}: one closed trade`);
  const s = stats || {};
  check(s.avgTradeDurationNs === 9e9, `${label}: avgTradeDurationNs (${s.avgTradeDurationNs})`);
  check(s.medianTradeDurationNs === 9e9,
        `${label}: medianTradeDurationNs (${s.medianTradeDurationNs})`);
  check(s.maxTradeDurationNs === 9e9, `${label}: maxTradeDurationNs (${s.maxTradeDurationNs})`);
  check(typeof s.timeWeightedReturn === 'number' && s.timeWeightedReturn > 0,
        `${label}: timeWeightedReturn (${s.timeWeightedReturn})`);
  check(typeof s.startTimeNs === 'bigint' && s.startTimeNs === T0,
        `${label}: startTimeNs is the first fill, to the nanosecond (${s.startTimeNs})`);
  check(typeof s.endTimeNs === 'bigint' && s.endTimeNs === T0 + 9n * SEC,
        `${label}: endTimeNs is the last fill, to the nanosecond (${s.endTimeNs})`);
}

function failure(fn) {
  try { fn(); }
  catch (e) { return String(e && e.message ? e.message : e); }
  return null;
}

console.log('=== a failed run says why ===');
{
  const missing = path.join(root, 'no_such_tape');
  let msg = failure(() => roundTripRunner().runTape(missing));
  check(msg !== null, 'runTape on a missing directory throws');
  check(msg !== null && msg.includes('does not exist') && msg.includes(missing),
        `runTape names the reason and the path (${msg})`);

  msg = failure(() => roundTripRunner().runTapes([tape, missing]));
  check(msg !== null && msg.includes('not a directory') && msg.includes(missing),
        `runTapes names the reason and the path (${msg})`);

  msg = failure(() => roundTripRunner().runTapes([]));
  check(msg !== null && msg.includes('empty'), `runTapes([]) names the reason (${msg})`);

  const missingCsv = path.join(root, 'no_such.csv');
  msg = failure(() => roundTripRunner().runCsv(missingCsv, 'AAA'));
  check(msg !== null, 'runCsv on a missing file throws');
  check(msg !== null && msg.includes('cannot open') && msg.includes(missingCsv),
        `runCsv names the reason and the path (${msg})`);
  check(msg !== null && !msg.includes('lrvx_backtest_runner_run_csv'),
        `runCsv does not name the C function (${msg})`);

  const badCsv = path.join(root, 'bad.csv');
  fs.writeFileSync(badCsv, 'timestamp,open,high,low,close,volume\n1767225600,1,1,1,1,1\nx,1,1,1,1,1\n');
  msg = failure(() => roundTripRunner().runCsv(badCsv, 'AAA'));
  check(msg !== null && msg.includes(badCsv) && msg.includes('line 3'),
        `runCsv names the file and the line of a bad row (${msg})`);

  // A volume_impact runner refuses a replay that carries no book depth; the
  // refusal is thrown with its reason, not returned as null.
  const depthRunner = () => {
    const reg = new lrvx.SymbolRegistry();
    reg.addSymbol('test', 'AAA', 0.01);
    return new lrvx.BacktestRunner(reg, 0, 10000,
                                   {slippageModel: 'volume_impact', slippageImpactCoeff: 0.5});
  };
  const one = new BigInt64Array([T0]);
  const px = new Float64Array([100]);
  for (const [label, run] of [
    ['runOhlcv', (bt) => bt.runOhlcv(one, px, 'AAA')],
    ['runBars', (bt) => bt.runBars(one, one, px, px, px, px, px, 'AAA')],
    ['runCsv', (bt) => bt.runCsv(missingCsv, 'AAA')],
  ]) {
    let result;
    msg = failure(() => { result = run(depthRunner()); });
    check(msg !== null && msg.includes('volume_impact') && result === undefined,
          `${label} on a refused run throws the reason (${msg})`);
  }
}

fs.rmSync(root, {recursive: true, force: true});

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed > 0 ? 1 : 0);
