'use strict';
/**
 * node/test/test_runner_strategy_ids.js -- strategies added to one Runner hold
 * different subscriber ids, so their order ids, (id << 32) + n, never meet.
 * A Runner assigns the ids itself, in the order the strategies are added.
 *
 * Run from repo root:
 *   cd node && node test/test_runner_strategy_ids.js
 */

const path = require('path');
const lrvx = require(path.join(__dirname, '..'));

let passed = 0;
let failed = 0;
function check(cond, msg) {
  if (cond) { passed++; console.log(`  ok  ${msg}`); }
  else { failed++; console.error(`  FAIL  ${msg}`); }
}

const TWO_32 = 4294967296;

function twoStrategies(threaded) {
  const reg = new lrvx.SymbolRegistry();
  const sym = reg.addSymbol('test', 'BTCUSDT', 0.01);
  const ids = {a: [], b: []};
  const runner = new lrvx.Runner(reg, () => {}, threaded);
  const strategy = (bucket) => ({
    symbols: [sym],
    onTrade(_ctx, _trade, emit) { bucket.push(emit.limitBuy(100, 1)); },
  });
  runner.addStrategy(strategy(ids.a));
  runner.addStrategy(strategy(ids.b));
  return {runner, sym, ids};
}

console.log('=== two strategies on a synchronous Runner ===');
{
  const {runner, sym, ids} = twoStrategies(false);
  runner.start();
  runner.onTrade(Number(sym), 100, 1, true, 1000n);
  runner.onTrade(Number(sym), 100, 1, true, 2000n);
  runner.stop();
  check(ids.a.length === 2 && ids.b.length === 2, 'each strategy sent two orders');
  const all = new Set([...ids.a, ...ids.b]);
  check(all.size === 4, `no order id is shared (${[...ids.a, ...ids.b].join(', ')})`);
  check(Math.floor(ids.a[0] / TWO_32) === 1, `the first strategy holds id 1 (${ids.a[0]})`);
  check(Math.floor(ids.b[0] / TWO_32) === 2, `the second holds id 2 (${ids.b[0]})`);
  check(ids.a[0] % TWO_32 === ids.b[0] % TWO_32, 'each counts its own orders from 1');
}

console.log('=== the same object added twice is two strategies ===');
{
  const reg = new lrvx.SymbolRegistry();
  const sym = reg.addSymbol('test', 'BTCUSDT', 0.01);
  const seen = [];
  const one = {symbols: [sym], onTrade(_ctx, _trade, emit) { seen.push(emit.limitBuy(100, 1)); }};
  const runner = new lrvx.Runner(reg, () => {}, false);
  let threw = '';
  try {
    runner.addStrategy(one);
    runner.addStrategy(one);
  } catch (e) { threw = String(e.message); }
  check(threw === '', `addStrategy takes it both times (${threw})`);
  runner.start();
  runner.onTrade(Number(sym), 100, 1, true, 1000n);
  runner.stop();
  check(seen.length === 2 && seen[0] !== seen[1], `with different order ids (${seen.join(', ')})`);
}

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed > 0 ? 1 : 0);
