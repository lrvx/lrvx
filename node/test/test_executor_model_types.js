'use strict';
/**
 * node/test/test_executor_model_types.js
 *
 * SimulatedExecutor.setDefaultSlippage and setQueueModel take a model by name
 * (a string) or by its exported SLIPPAGE_* / QUEUE_* code (a number). Anything
 * else was turned into a string with ToString first, so ['fixed_bps'], or an
 * object whose toString spelled a model, was taken as that model and
 * undefined was reported as the unknown model 'undefined'. Each now throws a
 * TypeError naming the parameter and the forms it takes.
 *
 * Run from repo root:
 *   cd node && node test/test_executor_model_types.js
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

const spells = (name) => ({ toString: () => name });

for (const [what, v] of [["['fixed_bps']", ['fixed_bps']], ['an object spelling fixed_bps',
                                                            spells('fixed_bps')],
                         ['undefined', undefined], ['null', null], ['true', true]]) {
  refuses(() => new lrvx.SimulatedExecutor().setDefaultSlippage(v, 0, 0, 10),
          'setDefaultSlippage: model must be one of', `setDefaultSlippage refuses ${what}`);
}
for (const [what, v] of [["['tob']", ['tob']], ['an object spelling tob', spells('tob')],
                         ['undefined', undefined], ['true', true]]) {
  refuses(() => new lrvx.SimulatedExecutor().setQueueModel(v, 1),
          'setQueueModel: model must be one of', `setQueueModel refuses ${what}`);
}
takes(() => new lrvx.SimulatedExecutor().setDefaultSlippage('fixed_bps', 0, 0, 10),
      'setDefaultSlippage takes a name');
takes(() => new lrvx.SimulatedExecutor().setDefaultSlippage(lrvx.SLIPPAGE_FIXED_BPS, 0, 0, 10),
      'setDefaultSlippage takes a code');
takes(() => new lrvx.SimulatedExecutor().setQueueModel('tob', 1), 'setQueueModel takes a name');
takes(() => new lrvx.SimulatedExecutor().setQueueModel(lrvx.QUEUE_TOB, 1),
      'setQueueModel takes a code');
{
  let e = null;
  try { new lrvx.SimulatedExecutor().setDefaultSlippage('bogus'); } catch (x) { e = x; }
  check(e && /got 'bogus'/.test(e.message), 'an unknown name still names it');
}

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed === 0 ? 0 : 1);
