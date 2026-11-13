'use strict';
/**
 * node/test/test_slippage_model_option_type.js
 *
 * BacktestRunner's slippageModel option is a model name. It was turned into a
 * string with ToString first, so ['fixed_bps'], or an object whose toString
 * spelled a model, was taken as that model and null as the unknown model
 * 'null'. Anything but a string now throws a TypeError naming the option.
 *
 * Run from repo root:
 *   cd node && node test/test_slippage_model_option_type.js
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
    check(e instanceof TypeError && e.message.startsWith(expected),
          `${msg} (${e.constructor.name}: ${e.message})`);
  }
}
function takes(fn, msg) {
  try { fn(); check(true, msg); } catch (e) { check(false, `${msg} (${e.message})`); }
}

const reg = new lrvx.SymbolRegistry();
reg.addSymbol('test', 'BTCUSDT', 0.01);
const spells = (name) => ({ toString: () => name });

for (const [what, v] of [["['fixed_bps']", ['fixed_bps']],
                         ['an object spelling fixed_bps', spells('fixed_bps')], ['null', null]]) {
  refuses(() => new lrvx.BacktestRunner(reg, 0, 1000, { slippageModel: v, slippageBps: 10 }),
          'BacktestRunner: slippageModel must be a string',
          `BacktestRunner refuses ${what} as slippageModel`);
}
takes(() => new lrvx.BacktestRunner(reg, 0, 1000, { slippageModel: 'fixed_bps', slippageBps: 10 }),
      'BacktestRunner takes a slippageModel name');

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed === 0 ? 0 : 1);
