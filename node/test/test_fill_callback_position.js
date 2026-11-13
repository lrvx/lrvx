'use strict';
/**
 * node/test/test_fill_callback_position.js -- ctx.position inside onFill and
 * onOrderUpdate already counts the fill being delivered.
 *
 * Run from repo root:
 *   cd node && node test/test_fill_callback_position.js
 */

const path = require('path');
const lrvx = require(path.join(__dirname, '..'));

let passed = 0;
let failed = 0;
function check(cond, msg) {
  if (cond) { passed++; console.log(`  ok  ${msg}`); }
  else { failed++; console.error(`  FAIL  ${msg}`); }
}
const near = (a, b) => Math.abs(a - b) < 1e-9;

const SEC = 1000000000n;
const PRICES = [100.0, 100.5, 101.0, 100.5, 100.0, 99.5, 100.0, 100.5];

const reg = new lrvx.SymbolRegistry();
const sym = reg.addSymbol('sim', 'BTCUSDT', 0.5);
const bt = new lrvx.BacktestRunner(reg, 0, 100000);

// Buys 1, sells 0.4, then closes; a parked limit is cancelled at the end.
let seen = 0;
let parked = 0;
const fills = [];
const updates = [];
bt.setStrategy({
  symbols: [sym],
  onTrade(_ctx, _trade, emit) {
    seen += 1;
    if (seen === 1) {
      emit.marketBuy(1);
      parked = emit.limitBuy(50, 1);
    } else if (seen === 3) {
      emit.marketSell(0.4);
    } else if (seen === 5) {
      emit.closePosition();
    } else if (seen === 7) {
      emit.cancel(parked);
    }
  },
  onFill(ctx, ev) { fills.push({side: ev.side, qty: ev.fillQty, position: ctx.position}); },
  onOrderUpdate(ctx, ev) {
    updates.push({status: ev.status, position: ctx.position, fillsBefore: fills.length});
  },
});

const ts = new BigInt64Array(PRICES.length);
for (let i = 0; i < PRICES.length; i++) { ts[i] = BigInt(i + 1) * SEC; }
bt.runOhlcv(ts, new Float64Array(PRICES), 'BTCUSDT');

console.log('=== ctx.position in onFill is the position after the fill ===');
check(fills.length === 3, `three fills (${fills.length})`);
const after = [1.0, 0.6, 0.0];
fills.forEach((f, i) => {
  check(near(f.position, after[i]),
        `${f.side} of ${f.qty}: ctx.position ${f.position}, want ${after[i]}`);
});

console.log('=== ctx.position in onOrderUpdate is current ===');
const canceled = updates.filter((u) => u.status === 'CANCELED');
check(canceled.length === 1, `the parked limit was cancelled (${updates.map((u) => u.status)})`);
check(canceled.length === 1 && near(canceled[0].position, 0),
      `flat by then (${canceled.length === 1 ? canceled[0].position : '-'})`);
// The parked limit is acknowledged on the same trade as the entry, after it filled.
const afterEntry = updates.filter((u) => u.fillsBefore === 1);
check(afterEntry.length > 0 && near(afterEntry[0].position, 1),
      `the first update after the entry fill shows it (${afterEntry.map((u) => u.position)})`);

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed > 0 ? 1 : 0);
