'use strict';
/**
 * node/test/test_argument_types.js
 *
 *   - A clock reading or a count taken as a Number or a BigInt: a BigInt
 *     past 64 signed bits is a RangeError (it used to wrap), and a value that
 *     is neither is a TypeError naming the argument (it used to be a bare
 *     "A number was expected").
 *   - SymbolRegistry.addSymbol and the BacktestRunner, Runner and
 *     WalkForwardRunner constructors check what they are given: a TypeError
 *     naming the argument, where they used to read a wrong type as an empty
 *     string, take a non-number tick as "no tick", or throw "Invalid
 *     argument" for an object that is not a SymbolRegistry.
 *   - Arguments read as typed arrays are checked for their element type, and
 *     a TypeError thrown by a check keeps its class through the wrapper that
 *     turns native errors into JS ones (it became a plain Error).
 *
 * Run from repo root:
 *   cd node && node test/test_argument_types.js
 */

const path = require('path');
const lrvx = require(path.join(__dirname, '..'));

let passed = 0;
let failed = 0;
function check(cond, msg) {
  if (cond) { passed++; console.log(`  ok  ${msg}`); }
  else { failed++; console.error(`  FAIL  ${msg}`); }
}
function throwsNaming(fn, Kind, word, msg) {
  try {
    fn();
    check(false, `${msg}: nothing was thrown`);
  } catch (e) {
    check(e instanceof Kind && String(e.message).includes(word),
          `${msg} (${e.constructor.name}: ${e.message})`);
  }
}

console.log('=== clock readings ===');
{
  const ex = new lrvx.SimulatedExecutor();
  ex.advanceClock(5n);
  ex.advanceClock(6);
  check(true, 'a Number and a BigInt in range are taken');
  throwsNaming(() => ex.advanceClock(2n ** 63n), RangeError, 'advanceClock: timestampNs',
               'a BigInt of 2^63 is a RangeError');
  throwsNaming(() => ex.advanceClock(-(2n ** 63n) - 1n), RangeError, 'advanceClock: timestampNs',
               'a BigInt below -2^63 is a RangeError');
  throwsNaming(() => ex.advanceClock('7'), TypeError, 'advanceClock: timestampNs',
               'a string is a TypeError naming the argument');
  throwsNaming(() => ex.advanceClock(NaN), TypeError, 'advanceClock: timestampNs',
               'NaN is a TypeError naming the argument');
  throwsNaming(() => ex.advanceClock(), TypeError, 'advanceClock: timestampNs',
               'a missing reading is a TypeError naming the argument');

  const acct = new lrvx.Account(1, 1000);
  throwsNaming(() => acct.hasStaleMarks(10n, 2n ** 64n), RangeError, 'hasStaleMarks: budgetNs',
               'a count read like a clock refuses a BigInt past 64 bits');
}

console.log('=== SymbolRegistry.addSymbol ===');
{
  const reg = new lrvx.SymbolRegistry();
  throwsNaming(() => reg.addSymbol(1, 'BTCUSDT'), TypeError, 'addSymbol: exchange',
               'a Number exchange');
  throwsNaming(() => reg.addSymbol('binance', {}), TypeError, 'addSymbol: symbol',
               'an object symbol');
  throwsNaming(() => reg.addSymbol('binance', 'BTCUSDT', '0.1'), TypeError, 'addSymbol: tickSize',
               'a string tick is refused, not taken as "no tick"');
  throwsNaming(() => reg.addSymbol('binance', 'BTCUSDT', 0.1, '1'), TypeError, 'addSymbol: lotSize',
               'a string lot is refused, not taken as "no lot"');
  const s = reg.addSymbol('binance', 'BTCUSDT', undefined, undefined);
  check(!reg.tickSizeDeclared(s) && s.lotSize === null, 'undefined tick and lot are left out');
  const t = reg.addSymbol('binance', 'ETHUSDT', 0.01, 0.001);
  check(reg.tickSizeDeclared(t) && t.lotSize === 0.001, 'a Number tick and lot are taken');
}

console.log('=== runner constructors ===');
{
  const reg = new lrvx.SymbolRegistry();
  throwsNaming(() => new lrvx.BacktestRunner({}, 0, 1000), TypeError, 'BacktestRunner: registry',
               'BacktestRunner: a plain object is not a registry');
  throwsNaming(() => new lrvx.BacktestRunner(new lrvx.Account(1, 1), 0, 1000), TypeError,
               'BacktestRunner: registry',
               'BacktestRunner: another wrapped class is not a registry');
  throwsNaming(() => new lrvx.BacktestRunner('reg', 0, 1000), TypeError, 'BacktestRunner: registry',
               'BacktestRunner: a string is not a registry');
  throwsNaming(() => new lrvx.BacktestRunner(reg, 0, 1000, 5), TypeError, 'BacktestRunner: options',
               'BacktestRunner: options that are not an object');
  throwsNaming(() => new lrvx.Runner({}, () => {}), TypeError, 'Runner: registry',
               'Runner: a plain object is not a registry');
  if (typeof lrvx.WalkForwardRunner === 'function') {
    throwsNaming(() => new lrvx.WalkForwardRunner({}, 0, 1000, {}), TypeError,
                 'WalkForwardRunner: registry',
                 'WalkForwardRunner: a plain object is not a registry');
  }
  const r = new lrvx.BacktestRunner(reg, 0, 1000);
  check(r !== null, 'a SymbolRegistry is taken');
}

console.log('=== typed arrays ===');
{
  const reg = new lrvx.SymbolRegistry();
  reg.addSymbol('binance', 'BTCUSDT', 0.01);
  const r = new lrvx.BacktestRunner(reg, 0, 1000);
  throwsNaming(() => r.runOhlcv(new BigInt64Array([1n]), new Float32Array([1]), 'BTCUSDT'),
               TypeError, 'runOhlcv: closes',
               'a Float32Array where a Float64Array is read');
  throwsNaming(() => r.runOhlcv([1n], new Float64Array([1]), 'BTCUSDT'), TypeError,
               'runOhlcv: timestamps', 'a plain array where a BigInt64Array is read');
  throwsNaming(() => r.runOhlcv(new BigInt64Array([1n]), new Float64Array([1]), 3), TypeError,
               'runOhlcv: symbol', 'a Number where a symbol name is read');
}

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
