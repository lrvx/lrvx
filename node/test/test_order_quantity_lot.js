'use strict';
/**
 * node/test/test_order_quantity_lot.js -- a lot passed to addSymbol is the
 * venue's quantity step, and a backtest refuses an order whose quantity is
 * not a whole number of it with rejectReason "quantity_off_lot". A symbol
 * added without a lot is not checked.
 *
 * Run from repo root:
 *   cd node && node test/test_order_quantity_lot.js
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

// Sends one limit buy of `qty` at 99.0 on the first bar and returns every
// order update the strategy saw.
function run(addArgs, qty) {
  const reg = new lrvx.SymbolRegistry();
  const sym = reg.addSymbol(...addArgs);
  const btr = new lrvx.BacktestRunner(reg, 0.0, 10000.0);
  const updates = [];
  let bar = 0;
  btr.setStrategy({
    symbols: [sym],
    onBar(_ctx, _b, emit) {
      if (bar++ === 0) { emit.limitBuy(99.0, qty, Number(sym)); }
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

console.log('=== addSymbol with a lot checks order quantities ===');
{
  const updates = run(['test', 'BTC', 0.1, 0.001], 0.0015);
  const rejected = updates.find(u => u.status === 'REJECTED');
  check(rejected !== undefined,
        `the off-lot limit is rejected (statuses: ${updates.map(u => u.status).join(', ')})`);
  check(rejected && rejected.rejectReason === 'quantity_off_lot',
        `with quantity_off_lot (got ${rejected && rejected.rejectReason})`);

  const fine = run(['test', 'BTC', 0.1, 0.001], 0.002);
  check(!fine.some(u => u.status === 'REJECTED'),
        `an on-lot quantity is accepted (statuses: ${fine.map(u => u.status).join(', ')})`);
}

console.log('\n=== addSymbol without a lot checks no quantity ===');
{
  const updates = run(['test', 'BTC', 0.1], 0.00012345);
  check(!updates.some(u => u.status === 'REJECTED'),
        `nothing is rejected (statuses: ${updates.map(u => u.status).join(', ')})`);
}

console.log('\n=== the Symbol carries its lot ===');
{
  const reg = new lrvx.SymbolRegistry();
  const withLot = reg.addSymbol('test', 'BTC', 0.1, 0.001);
  const without = reg.addSymbol('test', 'ETH', 0.01);
  check(Math.abs(withLot.lotSize - 0.001) < 1e-12, `lotSize is 0.001 (got ${withLot.lotSize})`);
  check(without.lotSize === null, `lotSize is null without one (got ${without.lotSize})`);

  let threw = false;
  try { reg.addSymbol('test', 'SOL', 0.01, 0); } catch (e) { threw = e instanceof RangeError; }
  check(threw, 'a lot of 0 is a RangeError');
  check(reg.symbolCount() === 2, 'and registers nothing');
}

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed === 0 ? 0 : 1);
