'use strict';
/**
 * node/test/test_run_input_errors.js -- a run handed a file it cannot read or
 * a symbol name the registry does not hold throws, and the error says which.
 *
 * Run from repo root:
 *   cd node && node test/test_run_input_errors.js
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
function thrown(fn) {
  try { fn(); } catch (e) { return e; }
  return null;
}
function says(err, needle, msg) {
  check(err !== null && String(err.message).includes(needle),
        `${msg} (${err === null ? 'no error' : err.message})`);
}

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'lrvx-run-input-errors-'));
const missing = path.join(root, 'no_such_thing');
const csv = path.join(root, 'ok.csv');
{
  let body = 'timestamp,open,high,low,close,volume\n';
  for (let i = 1; i <= 12; i++) { body += `${i},1,1,1,${100 + i},1\n`; }
  fs.writeFileSync(csv, body);
}
const badCsv = path.join(root, 'bad.csv');
fs.writeFileSync(badCsv, 'timestamp,open,high,low,close,volume\n1,1,1,1,100,1\nnot-a-row\n');

function registry() {
  const reg = new lrvx.SymbolRegistry();
  const sym = reg.addSymbol('test', 'BTCUSDT', 0.01);
  return {reg, sym};
}

console.log('=== BacktestRunner: an unknown symbol is refused ===');
{
  const {reg, sym} = registry();
  const bt = new lrvx.BacktestRunner(reg, 0, 10000);
  let events = 0;
  bt.setStrategy({symbols: [sym], onTrade() { events++; }, onBar() { events++; }});

  says(thrown(() => bt.runCsv(csv, 'NOPE')), "'NOPE' is not registered", 'runCsv');
  const ts = new BigInt64Array([1000000000n, 2000000000n]);
  const px = new Float64Array([100, 101]);
  says(thrown(() => bt.runOhlcv(ts, px, 'NOPE')), "'NOPE' is not registered", 'runOhlcv');
  const ends = new BigInt64Array([2000000000n, 3000000000n]);
  says(thrown(() => bt.runBars(ts, ends, px, px, px, px, px, 'NOPE')),
       "'NOPE' is not registered", 'runBars');
  check(events === 0, `the strategy saw nothing of the refused runs (${events})`);

  const stats = bt.runCsv(csv, 'BTCUSDT');
  check(stats !== undefined && events === 12, `a registered name still runs (${events} events)`);
}

console.log('=== WalkForwardRunner.runCsv says why it produced nothing ===');
{
  const {reg, sym} = registry();
  const wf = new lrvx.WalkForwardRunner(reg, 0, 10000,
                                        {mode: 'sliding', trainSize: 4, testSize: 4, step: 4});
  let made = 0;
  wf.setStrategyFactory(() => { made++; return {symbols: [sym], onTrade() {}}; });

  says(thrown(() => wf.runCsv(missing, 'BTCUSDT')), missing, 'a missing file names the path');
  says(thrown(() => wf.runCsv(badCsv, 'BTCUSDT')), 'line 3', 'a bad row names the line');
  says(thrown(() => wf.runCsv(csv, 'NOPE')), "'NOPE' is not registered", 'an unknown symbol');
  check(made === 0, `no strategy was built for the refused runs (${made})`);

  const folds = wf.runCsv(csv, 'BTCUSDT');
  check(Array.isArray(folds) && folds.length === 2, `a readable file still runs (${folds.length} folds)`);

  const short = path.join(root, 'short.csv');
  fs.writeFileSync(short, 'timestamp,open,high,low,close,volume\n1,1,1,1,100,1\n');
  const none = thrown(() => wf.runCsv(short, 'BTCUSDT'));
  check(none === null, 'a file too short for one fold is zero folds, not an error');
}

console.log('=== readers and recorders say what they could not open ===');
{
  says(thrown(() => new lrvx.TraceReader(missing)), 'manifest', 'TraceReader');
  says(thrown(() => new lrvx.TraceReader(missing)), missing, 'TraceReader names the path');

  const left = path.join(root, 'left');
  fs.mkdirSync(left);
  const diff = thrown(() => lrvx.tapeDiff(left, missing));
  says(diff, missing, 'tapeDiff names the side that is not a directory');
  check(diff !== null && diff.code === 'E_IO_001', 'tapeDiff keeps its error code');

  const merged = thrown(() => new lrvx.MergedTapeReader([missing]));
  says(merged, missing, 'MergedTapeReader names the directory');

  const blocker = path.join(root, 'blocker');
  fs.writeFileSync(blocker, 'x');
  const rec = thrown(() => new lrvx.TraceRecorder({path: path.join(blocker, 'trace')}));
  check(rec !== null && rec.message.length > 'TraceRecorder: '.length &&
        !rec.message.includes('lrvx_run_recorder_create'),
        `TraceRecorder gives a reason without the C function name (${rec && rec.message})`);
}

fs.rmSync(root, {recursive: true, force: true});
console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed > 0 ? 1 : 0);
