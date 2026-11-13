'use strict';
/**
 * node/test/test_emit_checks_and_drop_reasons.js
 *
 *   - emit.cancel read its order id without looking at its type; the other
 *     emit functions did the same with prices and quantities. A wrong type is
 *     a TypeError naming the call, and nothing is emitted.
 *   - A signal the kill switch, the order validator or the risk manager drops
 *     leaves its reason in lastError(), read right after the emit. Every emit
 *     clears the last error first.
 *
 * The executor is an in-process object: nothing leaves the process.
 *
 * Run from repo root:
 *   cd node && node test/test_emit_checks_and_drop_reasons.js
 */

const path = require('path');
const lrvx = require(path.join(__dirname, '..'));

let passed = 0;
let failed = 0;
function check(cond, msg) {
  if (cond) { passed++; console.log(`  ok  ${msg}`); }
  else { failed++; console.error(`  FAIL  ${msg}`); }
}

// One step per trade. Each step gets (emit, results); what it returns or
// throws is kept, and lastError() is read right after it.
function run(steps, gates = {}) {
  const reg = new lrvx.SymbolRegistry();
  const sym = reg.addSymbol('test', 'BTCUSDT', 0.01);
  const exec = {submitted: [], cancelled: []};
  const signals = [];
  const runner = new lrvx.Runner(reg, (s) => signals.push(s), false);
  runner.setExecutor({
    submit(order) { exec.submitted.push(order); },
    cancel(id) { exec.cancelled.push(id); },
  });
  if (gates.killSwitch) { runner.setKillSwitch(gates.killSwitch); }
  if (gates.orderValidator) { runner.setOrderValidator(gates.orderValidator); }
  if (gates.riskManager) { runner.setRiskManager(gates.riskManager); }
  const results = [];
  const thrown = [];
  const errors = [];
  let i = 0;
  runner.addStrategy({
    symbols: [sym],
    onTrade(_ctx, _trade, emit) {
      if (i >= steps.length) { return; }
      const step = steps[i];
      i += 1;
      try {
        results.push(step(emit, results));
        thrown.push(null);
      } catch (e) {
        results.push(undefined);
        thrown.push(e);
      }
      errors.push(lrvx.lastError());
    },
  });
  runner.start();
  for (let n = 0; n < steps.length; n++) {
    runner.onTrade(Number(sym), 100, 1, true, BigInt(1000 + n));
  }
  runner.stop();
  return {exec, signals, results, thrown, errors, sym: Number(sym)};
}

console.log('=== emit.cancel checks its order id ===');
{
  const bad = ['12', undefined, null, {}, [1], true, NaN, -1, 1.5, Infinity];
  const r = run([
    (emit) => emit.limitSell(101, 1),
    ...bad.map((v) => (emit) => emit.cancel(v)),
    (emit) => emit.cancel(),
    (emit, res) => emit.cancel(res[0]),
  ]);
  for (let k = 0; k < bad.length + 1; k++) {
    const e = r.thrown[1 + k];
    const what = k < bad.length ? String(bad[k]) : 'no argument';
    check(e instanceof TypeError && /emit\.cancel\(orderId\)/.test(e.message),
          `cancel(${what}) throws a TypeError naming the call (${e && e.message})`);
  }
  check(r.exec.cancelled.length === 1 && r.exec.cancelled[0] === r.results[0],
        `only the cancel with a real id reached the executor (${r.exec.cancelled.length})`);
  check(r.thrown[r.thrown.length - 1] === null, 'and it did not throw');
}

console.log('=== emit.cancel and emit.modify take the id as a BigInt too ===');
{
  const r = run([
    (emit) => emit.limitSell(101, 1),
    (emit, res) => emit.modify(BigInt(res[0]), 102, 1),
    (emit, res) => emit.cancel(BigInt(res[0])),
  ]);
  check(r.thrown.every((e) => e === null), `neither throws (${r.thrown.map(String)})`);
  check(r.exec.cancelled.length === 1 && r.exec.cancelled[0] === r.results[0],
        'the cancel reached the executor with the same id');
}

console.log('=== the other emit functions check their numbers ===');
{
  const calls = [
    ['marketBuy', (emit) => emit.marketBuy('1')],
    ['marketBuy', (emit) => emit.marketBuy()],
    ['marketSell', (emit) => emit.marketSell(null)],
    ['limitBuy', (emit) => emit.limitBuy('99', 1)],
    ['limitBuy', (emit) => emit.limitBuy(99)],
    ['limitSell', (emit) => emit.limitSell(101, {})],
    ['provideLiquidity', (emit) => emit.provideLiquidity(1, '2', 3)],
    ['withdrawLiquidity', (emit) => emit.withdrawLiquidity(undefined)],
    ['modify', (emit) => emit.modify('1', 100, 1)],
    ['modify', (emit) => emit.modify(1, '100', 1)],
  ];
  const r = run(calls.map((c) => c[1]));
  calls.forEach(([name], k) => {
    const e = r.thrown[k];
    check(e instanceof TypeError && e.message.includes('emit.' + name + '('),
          `emit.${name} with a wrong type throws a TypeError naming it (${e && e.message})`);
  });
  check(r.signals.length === 0 && r.exec.submitted.length === 0, 'and nothing was emitted');
}

console.log('=== a dropped signal leaves its reason ===');
{
  const deny = {kill: false, validator: false, risk: false};
  const steps = [
    (emit) => emit.limitSell(101, 1),
    (emit) => { deny.risk = true; return emit.limitSell(101, 1); },
    (emit) => { deny.validator = true; return emit.limitSell(101, 1); },
    (emit) => { deny.kill = true; return emit.limitSell(101, 1); },
    (emit) => { deny.kill = deny.validator = deny.risk = false; return emit.marketBuy(1); },
  ];
  const r = run(steps, {
    killSwitch: {check: () => !deny.kill},
    orderValidator: {validate: () => !deny.validator},
    riskManager: {allow: () => !deny.risk},
  });
  check(r.errors[0] === '', `an emit that passes leaves no error ('${r.errors[0]}')`);
  for (const [k, gate, token] of [[1, 'risk manager', 'risk_manager'],
                                  [2, 'order validator', 'order_validator'],
                                  [3, 'kill switch', 'kill_switch']]) {
    const why = r.errors[k];
    check(why.endsWith('[' + token + ']') && why.includes(gate) &&
          why.includes(String(r.results[k])) && why.includes('limit'),
          `dropped by the ${gate}: ${why}`);
  }
  check(r.errors[4] === '', `the next emit that passes clears it ('${r.errors[4]}')`);
  check(r.exec.submitted.length === 2 && r.signals.length === 2,
        `only the two that passed were sent (${r.exec.submitted.length})`);
}

console.log('=== every emit clears a stale reason ===');
{
  const stale = (fn) => (emit, res) => {
    lrvx.recompress('/no/such/segment.lrvx', '/no/such/out.lrvx', 'lz4');
    if (lrvx.lastError() === '') { throw new Error('the stale error was not set'); }
    return fn(emit, res);
  };
  const names = ['marketBuy', 'marketSell', 'limitBuy', 'limitSell', 'closePosition', 'cancel'];
  const r = run([
    stale((emit) => emit.marketBuy(1)),
    stale((emit) => emit.marketSell(1)),
    stale((emit) => emit.limitBuy(99, 1)),
    stale((emit) => emit.limitSell(101, 1)),
    stale((emit) => emit.closePosition()),
    stale((emit, res) => emit.cancel(res[3])),
  ]);
  names.forEach((name, k) => {
    check(r.thrown[k] === null && r.errors[k] === '',
          `emit.${name} leaves '' behind ('${r.errors[k]}', ${r.thrown[k]})`);
  });
}

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed === 0 ? 0 : 1);
