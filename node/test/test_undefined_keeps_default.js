'use strict';
/**
 * node/test/test_undefined_keeps_default.js
 *
 * An optional option or argument set to undefined keeps its default, the
 * same as leaving it out. Many reads asked whether the key was present
 * (`opts.Has(k)`) or the argument was passed (`info.Length() > n`) and then
 * read it with a checked reader, so `{ timeoutMs: undefined }` -- what a
 * spread of a partly filled config gives -- threw "must be a number" instead.
 * A value of the wrong type still throws.
 *
 * Run from repo root:
 *   cd node && node test/test_undefined_keeps_default.js
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
function keeps(fn, msg) {
  try {
    fn();
    check(true, msg);
  } catch (e) {
    check(false, `${msg} (${e.constructor.name}: ${e.message})`);
  }
}
function refuses(fn, msg) {
  try {
    fn();
    check(false, `${msg}: nothing was thrown`);
  } catch (e) {
    check(e instanceof TypeError, `${msg} (${e.constructor.name}: ${e.message})`);
  }
}

const U = undefined;
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'lrvx-undef-'));
try {
  console.log('=== option keys set to undefined ===');
  keeps(() => new lrvx.MultiFeedClock({ symbols: [1, 2], policy: U, timeoutMs: U,
                                        leaderSymbol: U, stalenessBudgetMs: U }),
        'MultiFeedClock');
  keeps(() => {
    const c = new lrvx.ConstantLatency({ feedNs: U, orderNs: 5, fillNs: U, seed: U });
    if (c.orderDelay() !== 5 || c.feedDelay() !== 0) throw new Error('defaults changed');
  }, 'ConstantLatency (latency getInt64)');
  keeps(() => new lrvx.GaussianLatency({ feedMeanNs: 10, feedStdNs: U }),
        'GaussianLatency (latency getDouble)');
  keeps(() => new lrvx.EmpiricalLatency({ feedSamples: [1, 2], orderSamples: U, fillSamples: U }),
        'EmpiricalLatency');
  keeps(() => {
    const g = new lrvx.OrderGroup({ parentSignalId: U, policy: U });
    g.setRiskLimits({ maxGrossNotional: U, maxConcentrationPct: U, maxLegQty: U });
    g.precheckSubmission({ equity: U, marketRefPrices: U });
    g.pairLatencyDecision({ leaderSubmitTsNs: 0, leaderAckTsNs: 0, ackReceived: U });
  }, 'OrderGroup');
  keeps(() => new lrvx.TWAPExecutor({ targetQty: 4, side: 'buy', durationNs: 4e9, sliceCount: 4,
                                      startTimeNs: 0, symbol: U, type: U, limitPrice: U }),
        'TWAPExecutor');
  keeps(() => new lrvx.DeltaBookEncoder({ anchorEvery: U }), 'DeltaBookEncoder');
  keeps(() => new lrvx.OrderJourneyTracer({ maxOrders: U, maxRecordsPerOrder: U, sampleRate: U,
                                            sampleSalt: U }),
        'OrderJourneyTracer');
  keeps(() => {
    const rec = new lrvx.TraceRecorder({ path: path.join(root, 'trace'), strategyId: U,
                                         strategyHash: U, runStartedNs: U });
    rec.writeSignal({ runTsNs: 1n, feedTsNs: U, signalId: U, flags: U, strengthRaw: U, name: U,
                      symbolIds: U, payload: U });
    rec.close();
  }, 'TraceRecorder and writeSignal');
  keeps(() => {
    const mk = (name) => {
      const dir = path.join(root, name);
      fs.mkdirSync(dir);
      const w = new lrvx.DataWriter(dir, U, U);
      w.writeTrade(1000n, 1000n, 100, 1, 1, 1, 0);
      w.close();
      return dir;
    };
    lrvx.tapeDiff(mk('left'), mk('right'), { maxMismatches: U, fieldToleranceNs: U });
  }, 'tapeDiff (and DataWriter with undefined maxSegmentMb / exchangeId)');

  console.log('=== optional arguments passed as undefined ===');
  keeps(() => lrvx.ammCurveRaydiumCp(10n ** 18n, 10n ** 18n, 2500, U, U), 'ammCurveRaydiumCp');
  keeps(() => new lrvx.BacktestResult(U, U), 'BacktestResult');
  keeps(() => new lrvx.Engine(U, U), 'Engine');
  keeps(() => new lrvx.SimulatedExecutor().setSubmitAckLatency(10, U), 'setSubmitAckLatency');
  keeps(() => new lrvx.MACD(U, U, U), 'MACD');

  console.log('=== a wrong type still throws ===');
  refuses(() => new lrvx.MultiFeedClock({ symbols: [1], timeoutMs: '200' }),
          'MultiFeedClock timeoutMs as a string');
  refuses(() => new lrvx.ConstantLatency({ feedNs: '1' }), 'ConstantLatency feedNs as a string');
  refuses(() => new lrvx.OrderGroup().setRiskLimits({ maxLegQty: null }),
          'setRiskLimits maxLegQty as null');
  refuses(() => new lrvx.BacktestResult('100'), 'BacktestResult initialCapital as a string');
} finally {
  fs.rmSync(root, { recursive: true, force: true });
}

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed === 0 ? 0 : 1);
