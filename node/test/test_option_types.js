'use strict';
/**
 * node/test/test_option_types.js
 *
 * An option or optional argument of the wrong type is refused with a
 * TypeError naming it. Each of these was read only when it had the expected
 * type and otherwise skipped as if it had been left out, so the run went on
 * with the default: a strategy with `symbols: 'BTCUSDT'` ran on no symbols, a
 * walk-forward `trainSize: '100'` trained on nothing, `onTrade: 'log'` had no
 * callback. A setting given by an unknown name was taken as the first one.
 * Leaving one out, or passing undefined, still keeps its default.
 *
 * Run from repo root:
 *   cd node && node test/test_option_types.js
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
function refuses(fn, Kind, word, msg) {
  try {
    fn();
    check(false, `${msg}: nothing was thrown`);
  } catch (e) {
    check(e instanceof Kind && String(e.message).includes(word),
          `${msg} (${e.constructor.name}: ${e.message})`);
  }
}
function takes(fn, msg) {
  try {
    fn();
    check(true, msg);
  } catch (e) {
    check(false, `${msg} (${e.constructor.name}: ${e.message})`);
  }
}

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'lrvx-option-types-'));
const csv = path.join(root, 'bars.csv');
{
  let body = 'timestamp,open,high,low,close,volume\n';
  for (let i = 1; i <= 12; i++) { body += `${i},1,1,1,${100 + i},1\n`; }
  fs.writeFileSync(csv, body);
}
const reg = new lrvx.SymbolRegistry();
const sym = reg.addSymbol('test', 'BTCUSDT', 0.01);

console.log('=== a strategy\'s symbols ===');
{
  refuses(() => new lrvx.BacktestRunner(reg, 0, 10000).setStrategy({symbols: 'BTCUSDT'}),
          TypeError, 'symbols must be an array', 'BacktestRunner.setStrategy');
  refuses(() => new lrvx.Runner(reg, () => {}, false).addStrategy({symbols: sym}),
          TypeError, 'symbols must be an array', 'Runner.addStrategy');
  const wf = new lrvx.WalkForwardRunner(reg, 0, 10000,
                                        {mode: 'sliding', trainSize: 4, testSize: 4, step: 4});
  wf.setStrategyFactory(() => ({symbols: 'BTCUSDT', onTrade() {}}));
  refuses(() => wf.runCsv(csv, 'BTCUSDT'), TypeError, 'symbols must be an array',
          'WalkForwardRunner: the strategy a factory returns');
  wf.setStrategyFactory(() => 42);
  refuses(() => wf.runCsv(csv, 'BTCUSDT'), TypeError, 'must be an object',
          'WalkForwardRunner: a factory that returns no strategy');
  wf.setStrategyFactory(() => ({symbols: [sym], onTrade() {}}));
  const folds = wf.runCsv(csv, 'BTCUSDT');
  check(folds.length === 2, `a factory with an array of symbols still runs (${folds.length} folds)`);

  takes(() => new lrvx.BacktestRunner(reg, 0, 10000).setStrategy({onTrade() {}}),
        'a strategy with no symbols is taken');
}

console.log('=== a strategy\'s callbacks ===');
{
  refuses(() => new lrvx.BacktestRunner(reg, 0, 10000).setStrategy({symbols: [sym], onTrade: 'log'}),
          TypeError, 'onTrade must be a function', 'a callback that is not a function');
  takes(() => new lrvx.BacktestRunner(reg, 0, 10000).setStrategy({symbols: [sym], onTrade: null}),
        'a null callback is no callback');
}

console.log('=== WalkForwardRunner config ===');
{
  refuses(() => new lrvx.WalkForwardRunner(reg, 0, 10000, {trainSize: '4', testSize: 4}),
          TypeError, 'trainSize', 'trainSize as a string');
  refuses(() => new lrvx.WalkForwardRunner(reg, 0, 10000, {testSize: 4, step: '2'}),
          TypeError, 'step', 'step as a string');
  refuses(() => new lrvx.WalkForwardRunner(reg, 0, 10000, {testSize: 4, minTrainSize: true}),
          TypeError, 'minTrainSize', 'minTrainSize as a boolean');
  refuses(() => new lrvx.WalkForwardRunner(reg, 0, 10000, {testSize: 4, mode: 1}),
          TypeError, 'mode', 'mode as a number');
  refuses(() => new lrvx.WalkForwardRunner(reg, 0, 10000, {testSize: 4, mode: 'slide'}),
          TypeError, "mode must be 'anchored' or 'sliding'", 'an unknown mode');
  takes(() => new lrvx.WalkForwardRunner(reg, 0, 10000, {testSize: 4, trainSize: undefined}),
        'an undefined option keeps its default');
}

console.log('=== optional arguments ===');
{
  const ex = new lrvx.SimulatedExecutor();
  refuses(() => ex.submitOrder(1, 'buy', 100, 1, 5), TypeError, 'must be a string',
          'SimulatedExecutor.submitOrder: an order type that is not a string');
  refuses(() => ex.submitOrder(2, 'buy', 100, 1, 'limit', 1, 'gtc'), TypeError, 'must be an object',
          'SimulatedExecutor.submitOrder: options that are not an object');
  takes(() => ex.submitOrder(3, 'buy', 100, 1, undefined), 'an undefined order type is market');
  refuses(() => new lrvx.Account('1', 1000), TypeError, 'must be', 'Account: an id that is a string');
  takes(() => new lrvx.Account(undefined, undefined), 'Account with undefined arguments');
  refuses(() => new lrvx.OHLCBinAggregator(60_000_000_000n, 'yes'), TypeError, 'must be a boolean',
          'OHLCBinAggregator: bySymbol as a string');
  refuses(() => new lrvx.OHLCBinAggregator(60_000_000_000n, false, 99), RangeError, 'eventFilter',
          'OHLCBinAggregator: an event filter that is not one');
  refuses(() => new lrvx.OHLCBinAggregator(60_000_000_000n, false, 'trades'), TypeError,
          'eventFilter', 'OHLCBinAggregator: an event filter that is a string');
  refuses(() => new lrvx.OHLCBinAggregator(60_000_000_000n, false, undefined, 7), TypeError,
          'symbolFilter must be an array', 'OHLCBinAggregator: a symbol filter that is not an array');
  refuses(() => new lrvx.OHLCBinAggregator('60s'), TypeError, 'must be a finite Number or a BigInt',
          'OHLCBinAggregator: a bucket that is a string');
  takes(() => new lrvx.OHLCBinAggregator(60_000_000_000n, undefined, undefined, undefined),
        'OHLCBinAggregator with the optional arguments undefined');
  refuses(() => new lrvx.PeakAggregator('1s'), TypeError, 'must be an array',
          'PeakAggregator: windows that are not an array');
  refuses(() => new lrvx.ConstantLatency(5), TypeError, 'must be an object',
          'ConstantLatency: options that are not an object');
  refuses(() => new lrvx.EmpiricalLatency({feedSamples: 5}), TypeError, 'feedSamples must be an array',
          'EmpiricalLatency: samples that are not an array');
  refuses(() => lrvx.report.heatmapHtml([[1, 2]], {title: 5}), TypeError, 'title must be a string',
          'heatmapHtml: a title that is not a string');
  refuses(() => lrvx.report.heatmapHtml([[1, 2]], {rowLabels: 'a'}), TypeError,
          'rowLabels must be an array', 'heatmapHtml: row labels that are not an array');
  refuses(() => lrvx.report.heatmapHtml([[1, 2]], 'title'), TypeError, 'must be an object',
          'heatmapHtml: options that are not an object');
  takes(() => lrvx.report.heatmapHtml([[1, 2]], undefined), 'heatmapHtml with undefined options');
}

console.log('=== settings given by name ===');
{
  const acct = new lrvx.Account(1, 1000);
  refuses(() => acct.setMarginMode('isolate'), TypeError, "'cross', 'isolated'",
          'Account.setMarginMode: an unknown name');
  refuses(() => acct.setMarginMode(true), TypeError, "'cross', 'isolated'",
          'Account.setMarginMode: a boolean');
  takes(() => acct.setMarginMode('isolated'), 'a known margin mode');
  const liq = new lrvx.LiquidationEngine();
  refuses(() => liq.setAdlRanking('bynance'), TypeError, "'pnl_ratio'",
          'LiquidationEngine.setAdlRanking: an unknown name');
  liq.setAdlRanking('bybit');
  check(liq.adlRanking() === 'bybit', `a known ADL ranking is set (${liq.adlRanking()})`);
  refuses(() => liq.setMarkImpactModel('anchored'), TypeError, "'book_anchored'",
          'LiquidationEngine.setMarkImpactModel: an unknown name');
  refuses(() => new lrvx.RateLimitPolicy().addFamilyBucket('md', 'b', 1000, 1), TypeError,
          "'market_data'", 'RateLimitPolicy.addFamilyBucket: an unknown family');
}

console.log('=== Runner.onBar fields ===');
{
  const runner = new lrvx.Runner(reg, () => {}, false);
  runner.addStrategy({symbols: [sym], onBar() {}});
  runner.start();
  refuses(() => runner.onBar(sym, {close: '101'}), TypeError, 'onBar: close must be a number',
          'a close that is a string');
  refuses(() => runner.onBar(sym, {close: 101, barType: 'time'}), TypeError, 'onBar: barType',
          'a bar type that is a string');
  takes(() => runner.onBar(sym, {close: 101, startTimeNs: 1n, endTimeNs: 2n}), 'a well-formed bar');
  runner.stop();
}

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed === 0 ? 0 : 1);
