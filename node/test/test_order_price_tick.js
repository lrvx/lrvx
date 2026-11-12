'use strict';
/**
 * node/test/test_order_price_tick.js -- a tick passed to addSymbol is the
 * venue's, 0.01 included, and a backtest refuses a limit price off it with
 * rejectReason "price_off_tick". A symbol added without a tick keeps 0.01 for
 * its book and is not checked.
 *
 * Run from repo root:
 *   cd node && node test/test_order_price_tick.js
 */

const path = require('path');
const lrvx = require(path.join(__dirname, '..'));

let passed = 0;
let failed = 0;
function check(cond, msg) {
  if (cond) { passed++; console.log(`  ok  ${msg}`); }
  else { failed++; console.error(`  FAIL  ${msg}`); }
}

const MIN = 60_000_000_000n;

// Sends one limit buy at 100.005, half a cent off the grid, on the first bar,
// and returns every order update the strategy saw.
function run(addArgs) {
  const reg = new lrvx.SymbolRegistry();
  const sym = reg.addSymbol(...addArgs);
  const btr = new lrvx.BacktestRunner(reg, 0.0, 10000.0);
  const updates = [];
  let bar = 0;
  btr.setStrategy({
    symbols: [sym],
    onBar(_ctx, _b, emit) {
      if (bar++ === 0) { emit.limitBuy(100.005, 1.0, Number(sym)); }
    },
    onOrderUpdate(_ctx, ev) { updates.push(ev); },
  });
  btr.runBars(
    new BigInt64Array([0n, MIN]),
    new BigInt64Array([MIN, 2n * MIN]),
    new Float64Array([100.0, 100.0]),
    new Float64Array([100.5, 100.5]),
    new Float64Array([99.5, 99.5]),
    new Float64Array([100.0, 100.0]),
    new Float64Array([10.0, 10.0]),
    'BTC');
  return updates;
}

console.log('=== addSymbol with an explicit 0.01 tick checks order prices ===');
{
  const updates = run(['test', 'BTC', 0.01]);
  const rejected = updates.find(u => u.status === 'REJECTED');
  check(rejected !== undefined,
        `the off-tick limit is rejected (statuses: ${updates.map(u => u.status).join(', ')})`);
  check(rejected && rejected.rejectReason === 'price_off_tick',
        `with price_off_tick (got ${rejected && rejected.rejectReason})`);
}

console.log('\n=== addSymbol without a tick checks nothing ===');
{
  const updates = run(['test', 'BTC']);
  check(!updates.some(u => u.status === 'REJECTED'),
        `nothing is rejected (statuses: ${updates.map(u => u.status).join(', ')})`);
}

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed === 0 ? 0 : 1);
