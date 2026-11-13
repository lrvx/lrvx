'use strict';
/**
 * node/test/test_handle_kinds.js
 *
 * AmmCurveHandle, PoolTapeHandle and PoolReplayHandle are opaque handles of
 * the same JavaScript type. A handle of one kind passed where another is read
 * was handed to the C API as the other C type, and anything that is not a
 * handle threw a bare "Invalid argument". Each read now takes only its own
 * kind and throws a TypeError naming the parameter and the kind it expects.
 *
 * Run from repo root:
 *   cd node && node test/test_handle_kinds.js
 */

const path = require('path');
const lrvx = require(path.join(__dirname, '..'));

let passed = 0;
let failed = 0;
function check(cond, msg) {
  if (cond) { passed++; console.log(`  ok  ${msg}`); }
  else { failed++; console.error(`  FAIL  ${msg}`); }
}
function refuses(fn, expected, msg) {
  try {
    fn();
    check(false, `${msg}: nothing was thrown`);
  } catch (e) {
    check(e instanceof TypeError && String(e.message).includes(expected),
          `${msg} (${e.constructor.name}: ${e.message})`);
  }
}

const curve = lrvx.ammCurveConstantProduct(10n ** 21n, 2n * 10n ** 21n, 997, 1000);
const tape = lrvx.poolTapeCreate();
lrvx.poolTapeDescriptorConstantProduct(tape, 997, 1000, 18, 18);
lrvx.poolTapeCheckpoint(tape, 1, 10n ** 21n, 2n * 10n ** 21n);
const replay = lrvx.poolTapeReplay(tape, 0, 1, 18, 18);

// The right kinds still work.
check(lrvx.ammCurveTokenCount(curve) === 2, 'a curve reads as a curve');
check(lrvx.poolReplayTradeCount(replay) === 0, 'a replay reads as a replay');
check(lrvx.ammCurveTokenCount(lrvx.poolReplayCurve(replay)) === 2,
      'the curve a replay hands back reads as a curve');

const strangers = {
  curve: [['a PoolTapeHandle', tape], ['a PoolReplayHandle', replay],
          ['a plain object', {}], ['a number', 1], ['undefined', undefined]],
  tape: [['an AmmCurveHandle', curve], ['a PoolReplayHandle', replay],
         ['a plain object', {}], ['undefined', undefined]],
  replay: [['an AmmCurveHandle', curve], ['a PoolTapeHandle', tape],
           ['a plain object', {}], ['undefined', undefined]],
};

// [method, parameter, expected kind, call taking the handle]
const cases = [
  ['ammCurveTokenCount', 'curve', 'an AmmCurveHandle', (h) => lrvx.ammCurveTokenCount(h)],
  ['ammCurveAmountOut', 'curve', 'an AmmCurveHandle', (h) => lrvx.ammCurveAmountOut(h, 0, 1, 10n)],
  ['ammCurveApplySwap', 'curve', 'an AmmCurveHandle', (h) => lrvx.ammCurveApplySwap(h, 0, 1, 10n)],
  ['ammCurveBalance', 'curve', 'an AmmCurveHandle', (h) => lrvx.ammCurveBalance(h, 0)],
  ['ammCurveSqrtPrice', 'curve', 'an AmmCurveHandle', (h) => lrvx.ammCurveSqrtPrice(h)],
  ['ammCurveLiquidity', 'curve', 'an AmmCurveHandle', (h) => lrvx.ammCurveLiquidity(h)],
  ['ammCurveClone', 'curve', 'an AmmCurveHandle', (h) => lrvx.ammCurveClone(h)],
  ['poolTapeDescriptorConstantProduct', 'tape', 'a PoolTapeHandle',
   (h) => lrvx.poolTapeDescriptorConstantProduct(h, 997, 1000, 18, 18)],
  ['poolTapeDescriptorRaydiumCp', 'tape', 'a PoolTapeHandle',
   (h) => lrvx.poolTapeDescriptorRaydiumCp(h, 2500, 0, true, 9, 6)],
  ['poolTapeDescriptorClmm', 'tape', 'a PoolTapeHandle',
   (h) => lrvx.poolTapeDescriptorClmm(h, 0, 3000, 18, 6)],
  ['poolTapeCheckpoint', 'tape', 'a PoolTapeHandle',
   (h) => lrvx.poolTapeCheckpoint(h, 2, 10n, 10n)],
  ['poolTapeSwap', 'tape', 'a PoolTapeHandle', (h) => lrvx.poolTapeSwap(h, 3, true, 10n)],
  ['poolTapeReplay', 'tape', 'a PoolTapeHandle', (h) => lrvx.poolTapeReplay(h, 0, 1, 18, 18)],
  ['poolReplayDriftCount', 'replay', 'a PoolReplayHandle', (h) => lrvx.poolReplayDriftCount(h)],
  ['poolReplayTradeCount', 'replay', 'a PoolReplayHandle', (h) => lrvx.poolReplayTradeCount(h)],
  ['poolReplayCurve', 'replay', 'a PoolReplayHandle', (h) => lrvx.poolReplayCurve(h)],
];

for (const [method, param, kind, call] of cases) {
  for (const [what, value] of strangers[param]) {
    refuses(() => call(value), `${method}: ${param} must be ${kind}`,
            `${method} refuses ${what}`);
  }
}

// Nothing above moved the pool or the tape it was refused for.
check(lrvx.ammCurveBalance(curve, 0) === 10n ** 21n, 'the curve is untouched');
check(lrvx.poolReplayTradeCount(lrvx.poolTapeReplay(tape, 0, 1, 18, 18)) === 0,
      'the tape is untouched');

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed === 0 ? 0 : 1);
