'use strict';
/**
 * node/test/test_runner_order_closed.js -- the record a Runner keeps of the
 * orders it sent to its executor.
 *
 * emit.modify is sent as the order it names, so the runner keeps every order
 * that can rest. Nothing told it when one finished, so the record only grew,
 * and a modify for an id it did not hold went out as a made-up LIMIT / BUY
 * order with no symbol. The executor is an in-process object: nothing leaves
 * the process.
 *
 * Run from repo root:
 *   cd node && node test/test_runner_order_closed.js
 */

const path = require('path');
const lrvx = require(path.join(__dirname, '..'));

let passed = 0;
let failed = 0;
function check(cond, msg) {
  if (cond) { passed++; console.log(`  ok  ${msg}`); }
  else { failed++; console.error(`  FAIL  ${msg}`); }
}

// One step of `steps` per trade. Each step gets (emit, results) and its return
// value is kept; lastError() is read right after it. `between[i]` runs on the
// host side after step i.
function run(steps, between = {}) {
  const reg = new lrvx.SymbolRegistry();
  const sym = reg.addSymbol('test', 'BTCUSDT', 0.01);
  const exec = {submitted: [], replaced: [], cancelled: []};
  const runner = new lrvx.Runner(reg, () => {}, false);
  runner.setExecutor({
    submit(order) { exec.submitted.push(order); },
    replace(oldId, order) { exec.replaced.push({oldId, order}); },
    cancel(id) { exec.cancelled.push(id); },
  });
  const results = [];
  const errors = [];
  let i = 0;
  runner.addStrategy({
    symbols: [sym],
    onTrade(_ctx, _trade, emit) {
      if (i >= steps.length) { return; }
      results.push(steps[i](emit, results));
      errors.push(lrvx.lastError());
      i += 1;
    },
  });
  runner.start();
  for (let n = 0; n < steps.length; n++) {
    runner.onTrade(Number(sym), 100, 1, true, BigInt(1000 + n));
    if (between[n]) { between[n](runner, results); }
  }
  runner.stop();
  return {exec, results, errors, hookErrors: runner.hookErrors()};
}

console.log('=== a closed order is dropped and a later modify is not sent ===');
{
  const r = run([
    (emit) => emit.limitSell(101, 3),
    (emit, res) => emit.modify(res[0], 102, 3),
    (emit, res) => emit.modify(res[0], 103, 3),
  ], {1: (runner, res) => runner.orderClosed(res[0])});
  const id = r.results[0];
  check(r.exec.submitted.length === 1, 'the order reached the executor');
  check(r.exec.replaced.length === 1 && r.exec.replaced[0].oldId === id,
        `only the modify before the close reaches it (${r.exec.replaced.length})`);
  check(r.errors[1] === '', `that modify leaves no error ('${r.errors[1]}')`);
  check(r.errors[2].includes(String(id)) && r.errors[2].endsWith('[unknown_order]'),
        `the refused one says why: ${r.errors[2]}`);
  check(r.hookErrors.length === 0, 'and it is not an executor hook failure');
}

console.log('=== orderClosed takes a BigInt and ignores an id it never held ===');
{
  const r = run([
    (emit) => emit.limitSell(101, 3),
    (emit, res) => emit.modify(res[0], 102, 3),
  ], {0: (runner, res) => {
    runner.orderClosed(123456789);
    runner.orderClosed(BigInt(res[0]));
  }});
  check(r.exec.replaced.length === 0, 'closed through a BigInt id');
  check(r.errors[1].includes('unknown_order'), `so the modify is refused: ${r.errors[1]}`);

  const reg = new lrvx.SymbolRegistry();
  reg.addSymbol('test', 'BTCUSDT', 0.01);
  for (const threaded of [false, true]) {
    const runner = new lrvx.Runner(reg, () => {}, threaded);
    let threw = '';
    try { runner.orderClosed(1); runner.orderClosed(1n); } catch (e) { threw = String(e); }
    check(threw === '', `${threaded ? 'threaded' : 'sync'}: orderClosed on an empty record`);
    try { runner.orderClosed(); } catch (e) { threw = e.constructor.name; }
    check(threw === 'TypeError', `${threaded ? 'threaded' : 'sync'}: orderClosed() is a TypeError`);
  }
}

console.log('=== a modify of an id never sent is not forwarded ===');
{
  const r = run([(emit) => emit.modify(424242, 100, 1)]);
  check(r.exec.replaced.length === 0, 'nothing reached the executor');
  check(r.errors[0].includes('424242') && r.errors[0].includes('unknown_order'),
        `lastError() says why: ${r.errors[0]}`);
}

console.log('=== a modify after a cancel is not forwarded ===');
{
  const r = run([
    (emit) => emit.limitSell(101, 3),
    (emit, res) => emit.cancel(res[0]),
    (emit, res) => emit.modify(res[0], 102, 3),
  ]);
  check(r.exec.cancelled.length === 1, 'the cancel reached the executor');
  check(r.exec.replaced.length === 0, 'the modify did not');
  check(r.errors[2].includes('unknown_order'), `and says why: ${r.errors[2]}`);
}

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed > 0 ? 1 : 0);
