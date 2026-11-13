'use strict';
/**
 * node/test/test_emit_order_ids.js -- the order-emitting emit functions return
 * the id of the order they sent, the id its events then carry, so a strategy
 * can modify or cancel an order without waiting for an event to learn it.
 *
 * Run from repo root:
 *   cd node && node test/test_emit_order_ids.js
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

function backtest(onBar, barArgs) {
  const reg = new lrvx.SymbolRegistry();
  const sym = reg.addSymbol('test', 'BTCUSDT', 0.5);
  const bt = new lrvx.BacktestRunner(reg, 0, 10000);
  const out = {fills: [], events: []};
  let seen = 0;
  bt.setStrategy({
    symbols: [sym],
    onBar(ctx, bar, emit) { seen += 1; onBar(seen, emit, out); },
    onFill(_ctx, ev) { out.fills.push(ev); },
    onOrderUpdate(_ctx, ev) { out.events.push(ev); },
  });
  bt.runBars(...barArgs);
  return out;
}

console.log('=== every order-emitting function returns its order id ===');
{
  const ids = {};
  const r = backtest((seen, emit) => {
    if (seen !== 1) { return; }
    ids.limitBuy = emit.limitBuy(90, 1);
    ids.limitSell = emit.limitSell(110, 1);
    ids.marketBuy = emit.marketBuy(1);
    ids.marketSell = emit.marketSell(1);
    ids.closeFlat = emit.closePosition();
    ids.provideLiquidity = emit.provideLiquidity(90, 110, 5);
    ids.withdrawLiquidity = emit.withdrawLiquidity(5);
    ids.cancel = emit.cancel(ids.limitSell);
  }, bars(100.5, 99.5));

  const names = ['limitBuy', 'limitSell', 'marketBuy', 'marketSell',
                 'provideLiquidity', 'withdrawLiquidity'];
  for (const name of names) {
    check(typeof ids[name] === 'number' && Number.isSafeInteger(ids[name]) && ids[name] > 0,
          `emit.${name} returns a positive integer id (${typeof ids[name]} ${ids[name]})`);
  }
  check(new Set(names.map((n) => ids[n])).size === names.length, 'the ids are all different');
  check(ids.cancel === undefined, 'emit.cancel names an order and returns undefined');
  check(ids.closeFlat === 0, `emit.closePosition with nothing to close sends no order: 0 (${ids.closeFlat})`);

  const seenIds = new Set(r.events.concat(r.fills).map((e) => e.orderId));
  check(seenIds.has(ids.limitBuy), 'the limit buy id is the one its events carry');
  check(seenIds.has(ids.limitSell), 'the limit sell id is the one its events carry');
  check(seenIds.has(ids.marketBuy), 'the market buy id is the one its events carry');
  const canceled = r.events.filter((e) => e.status === 'CANCELED');
  check(canceled.length === 1 && canceled[0].orderId === ids.limitSell,
        'cancelling by the returned id cancels that order');
}

console.log('=== closePosition returns the id of the closing order ===');
{
  let closeId = null;
  const r = backtest((seen, emit) => {
    if (seen === 1) { emit.marketBuy(1); }
    else if (seen === 3) { closeId = emit.closePosition(); }
  }, bars(100.5, 99.5));
  check(typeof closeId === 'number' && closeId > 0, `a positive id (${closeId})`);
  check(r.fills.some((f) => f.orderId === closeId && f.side === 'sell'),
        'and the closing fill carries it');
}

console.log('=== modify by the returned id, no order event needed ===');
{
  let id = 0;
  const r = backtest((seen, emit) => {
    if (seen === 1) { id = emit.limitBuy(90, 1); }
    else if (seen === 3) { emit.modify(id, 99, 2); }
  }, bars(100.5, 98));
  check(r.fills.length === 1, `one fill (${r.events.map((e) => e.status).join(',')})`);
  const f = r.fills[0] || {};
  check(f.orderId === id, `the fill carries the returned id (${f.orderId} vs ${id})`);
  check(f.fillPrice === 99 && f.fillQty === 2,
        `it fills at the modified price and quantity (${f.fillPrice}, ${f.fillQty})`);
}

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed > 0 ? 1 : 0);
