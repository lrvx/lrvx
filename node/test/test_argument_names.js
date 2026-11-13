'use strict';
/**
 * node/test/test_argument_names.js
 *
 * A wrong argument is named in the error by its parameter name from
 * index.d.ts, "advanceClock: timestampNs must be ...", where it used to be
 * named by position ("advanceClock: argument 1"). A free function is named
 * as it is exported (aggregateTimeBars, not agg_time_bars) and a method as
 * the class declares it (setSubmitAckLatency, not setSubmitAck); helpers
 * shared by several methods name the method that was called.
 *
 * Run from repo root:
 *   cd node && node test/test_argument_names.js
 */

const path = require('path');
const lrvx = require(path.join(__dirname, '..'));

let passed = 0;
let failed = 0;
function check(cond, msg) {
  if (cond) { passed++; console.log(`  ok  ${msg}`); }
  else { failed++; console.error(`  FAIL  ${msg}`); }
}
function names(fn, label) {
  try {
    fn();
    check(false, `${label}: nothing was thrown`);
  } catch (e) {
    const m = String(e.message);
    check(m.startsWith(`${label} `) && !/argument \d/.test(m), `${label} (${m})`);
  }
}

const f32 = new Float32Array(4);
const reg = new lrvx.SymbolRegistry();
reg.addSymbol('test', 'BTCUSDT', 0.01);

const ex = new lrvx.SimulatedExecutor();
names(() => ex.advanceClock('soon'), 'advanceClock: timestampNs');
names(() => ex.setSubmitAckLatency('fast'), 'setSubmitAckLatency: latencyNs');
names(() => ex.setCancelAckLatency(1, 'x'), 'setCancelAckLatency: jitterNs');
names(() => ex.submitOrder(1, 'buy', 'cheap', 1), 'submitOrder: price');
names(() => new lrvx.Account(1, 'rich'), 'Account: equity');
names(() => lrvx.aggregateTimeBars(new Float64Array(1), new Float64Array(1),
                                   new Float64Array(1), new Uint8Array(1), 'minute'),
      'aggregateTimeBars: intervalSeconds');
names(() => lrvx.sma(f32, 3), 'sma: input');
names(() => lrvx.sma(new Float64Array(4), 'three'), 'sma: period');
names(() => lrvx.targets.future_return(f32, 1), 'targets.future_return: input');
names(() => lrvx.correlation(f32, new Float64Array(4)), 'correlation: x');
names(() => new lrvx.SMA('ten'), 'SMA: period');
names(() => new lrvx.Slope('ten'), 'Slope: length');
names(() => new lrvx.SMA(3).update('x'), 'update: value');
names(() => new lrvx.ATR('ten'), 'ATR: period');
names(() => new lrvx.ATR(3).update(1, 'low', 1), 'update: low');
const book = new lrvx.OrderBook(0.01);
names(() => book.applySnapshot(f32, f32, f32, f32), 'applySnapshot: bidPrices');
names(() => book.applyDelta(new Float64Array(1), f32, f32, f32), 'applyDelta: bidQtys');
names(() => book.getBids('all'), 'getBids: n');
const matrix = new lrvx.CompositeBookMatrix();
names(() => matrix.applyDelta(0, 'BTC', f32, f32, f32, f32), 'applyDelta: symbol');
names(() => reg.tickSize('BTCUSDT'), 'tickSize: symbol');
names(() => reg.lotSize({id: 'x'}), 'lotSize: symbol.id');
names(() => new lrvx.TWAPExecutor(5), 'TWAPExecutor: opts');
names(() => new lrvx.ConstantLatency(5), 'ConstantLatency: opts');
names(() => new lrvx.ConstantLatency().reset('seed'), 'reset: seed');
names(() => new lrvx.WalkForwardRunner(reg, 'free', 10000, {testSize: 4}),
      'WalkForwardRunner: feeRate');
names(() => new lrvx.Engine().run({}), 'run: signals');

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed === 0 ? 0 : 1);
