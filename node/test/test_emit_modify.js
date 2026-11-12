'use strict';
/**
 * node/test/test_emit_modify.js -- emit.modify in a backtest: the order keeps
 * its id, symbol and side, and fills at the new price for the new quantity.
 *
 * Run from repo root:
 *   cd node && node test/test_emit_modify.js
 */

const path = require('path');
const lrvx = require(path.join(__dirname, '..'));

let passed = 0;
let failed = 0;
function check(cond, msg) {
  if (cond) { passed++; console.log(`  ok  ${msg}`); }
  else { failed++; console.error(`  FAIL  ${msg}`); }
}

const T0 = 1788220800000000000n;
const STEP = 60000000000n;
const N = 6;

// Six bars around 100; only the fifth leaves the 99.5 .. 100.5 band.
function bars(spikeHigh, spikeLow) {
  const starts = new BigInt64Array(N);
  const ends = new BigInt64Array(N);
  const highs = new Float64Array(N).fill(100.5);
  const lows = new Float64Array(N).fill(99.5);
  for (let i = 0; i < N; i++) {
    starts[i] = T0 + STEP * BigInt(i);
    ends[i] = T0 + STEP * BigInt(i + 1);
  }
  highs[4] = spikeHigh;
  lows[4] = spikeLow;
  return [starts, ends, new Float64Array(N).fill(100), highs, lows,
          new Float64Array(N).fill(100), new Float64Array(N).fill(10), 'BTCUSDT'];
}

// Limit on the first bar, modify on the third, optional cancel on the fourth.
function run({side, restPx, newPx, newQty, cancelAfter, modify = true}, barArgs) {
  const reg = new lrvx.SymbolRegistry();
  const sym = reg.addSymbol('test', 'BTCUSDT', 0.5);
  const bt = new lrvx.BacktestRunner(reg, 0, 10000);
  const out = {sym: Number(sym), orderId: 0, fills: [], events: [], modifyReturn: null};
  let seen = 0;
  bt.setStrategy({
    symbols: [sym],
    onBar(_ctx, _bar, emit) {
      seen += 1;
      if (seen === 1) {
        if (side === 'buy') { emit.limitBuy(restPx, 1); }
        else { emit.limitSell(restPx, 1); }
      } else if (seen === 3 && modify) {
        out.modifyReturn = emit.modify(out.orderId, newPx, newQty);
      } else if (seen === 4 && cancelAfter) {
        emit.cancel(out.orderId);
      }
    },
    onFill(_ctx, ev) { out.fills.push(ev); },
    onOrderUpdate(_ctx, ev) {
      if (out.orderId === 0) { out.orderId = ev.orderId; }
      out.events.push(ev);
    },
  });
  bt.runBars(...barArgs);
  return out;
}

console.log('=== the modified order fills at the new price ===');
for (const c of [
  {side: 'buy', restPx: 90, newPx: 99, newQty: 2, bars: bars(100.5, 98)},
  {side: 'sell', restPx: 110, newPx: 101, newQty: 0.5, bars: bars(102, 99.5)},
]) {
  const unmodified = run({...c, modify: false}, c.bars);
  check(unmodified.fills.length === 0, `${c.side}: the order as placed never fills`);

  const r = run(c, c.bars);
  check(r.orderId !== 0, `${c.side}: the strategy learned its order id`);
  check(r.modifyReturn === undefined, `${c.side}: emit.modify returns undefined`);
  check(r.fills.length === 1,
        `${c.side}: one fill (${r.events.map((e) => e.status).join(',')})`);
  const f = r.fills[0] || {};
  check(f.status === 'FILLED', `${c.side}: FILLED (${f.status})`);
  check(f.orderId === r.orderId, `${c.side}: same order id (${f.orderId})`);
  check(f.symbolId === r.sym, `${c.side}: same symbol (${f.symbolId})`);
  check(f.side === c.side, `${c.side}: same side (${f.side})`);
  check(f.fillPrice === c.newPx, `${c.side}: fills at the new price (${f.fillPrice})`);
  check(f.fillQty === c.newQty, `${c.side}: fills the new quantity (${f.fillQty})`);
  check(r.events.every((e) => e.status !== 'FILLED' && e.status !== 'PARTIALLY_FILLED'),
        `${c.side}: the fill reached onFill only, not onOrderUpdate`);
}

console.log('=== the modified order cancels by the same id ===');
{
  const r = run({side: 'buy', restPx: 90, newPx: 99, newQty: 2, cancelAfter: true},
                bars(100.5, 98));
  const canceled = r.events.filter((e) => e.status === 'CANCELED');
  check(canceled.length === 1,
        `one CANCELED event (${r.events.map((e) => e.status).join(',')})`);
  check(canceled.length === 1 && canceled[0].orderId === r.orderId, 'for the id the strategy holds');
  check(r.fills.length === 0, 'and no fill');
}

console.log('=== an off-tick modify is refused and the order is left alone ===');
{
  const r = run({side: 'buy', restPx: 90, newPx: 99.25, newQty: 2}, bars(100.5, 98));
  const rejects = r.events.filter((e) => e.status === 'REPLACE_REJECTED');
  check(rejects.length === 1,
        `one REPLACE_REJECTED event (${r.events.map((e) => e.status).join(',')})`);
  check(r.fills.length === 0, 'and no fill');
}

console.log('=== argument checks ===');
{
  const reg = new lrvx.SymbolRegistry();
  const sym = reg.addSymbol('test', 'BTCUSDT', 0.5);
  const bt = new lrvx.BacktestRunner(reg, 0, 10000);
  let err = null;
  let done = false;
  bt.setStrategy({
    symbols: [sym],
    onBar(_ctx, _bar, emit) {
      if (done) { return; }
      done = true;
      try { emit.modify(1); } catch (e) { err = e; }
    },
  });
  bt.runBars(...bars(100.5, 99.5));
  check(err instanceof TypeError, `emit.modify with missing arguments throws TypeError (${err})`);
}

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed > 0 ? 1 : 0);
