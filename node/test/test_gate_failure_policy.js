'use strict';
/**
 * node/test/test_gate_failure_policy.js -- a pre-trade gate that fails
 * denies the order, says so, and does not unwind through C.
 *
 * The three pre-trade gates (RiskManager.allow, KillSwitch.check,
 * OrderValidator.validate) are C function pointers the engine calls
 * inline while a signal is in flight. node/src/hooks.h reaches straight
 * back into JS from those bridges, and two failure modes were unhandled:
 *
 *   - the JS gate throws. Under NODE_ADDON_API_CPP_EXCEPTIONS_ALL
 *     (node/CMakeLists.txt) Napi::FunctionReference::Call turns the
 *     pending JS exception into a C++ one, which then unwinds out of a
 *     bridge declared to C, straight through the engine frames that were
 *     mid-signal, and surfaces at whatever JS frame happens to be on the
 *     stack -- `emit.marketBuy()` inside the strategy's own callback.
 *   - the JS gate returns something that is not a boolean. The bridge
 *     folded that into a deny with no signal to anyone, and the Python
 *     binding folded the same case the other way, into an allow.
 *
 * The policy, one for every binding: a host-language throw and a
 * non-boolean return both DENY the order, and both are reported. Neither
 * ever lets the order through, and neither escapes through the C
 * boundary. A gate that plainly returns false is a decision, not a
 * failure, and is not reported.
 *
 * Reported here means `runner.hookErrors()`: an array of
 * `{ hook, method, message }` records accumulated on the runner, oldest
 * first, readable synchronously after the call that failed. The process-
 * wide log callback is asynchronous and shared, so it cannot say which
 * runner denied what; a per-runner accessor can.
 *
 * Run from repo root:
 *   cd node && node test/test_gate_failure_policy.js
 */

const { spawnSync } = require('child_process');
const fs = require('fs');
const path = require('path');
const lrvx = require(path.join(__dirname, '..'));

// The child process re-runs the scenarios only to prove the process
// survives them, so it keeps its verdicts to itself.
const QUIET = process.argv[2] === '--child';

let passed = 0;
let failed = 0;
function check(cond, msg) {
  if (cond) { passed++; if (!QUIET) { console.log(`  ok  ${msg}`); } }
  else { failed++; if (!QUIET) { console.error(`  FAIL  ${msg}`); } }
}

const THROW_TEXT = 'boom from gate';

// setter -> the method name the hook object carries.
const GATES = [
  ['setRiskManager', 'allow', 'riskManager'],
  ['setKillSwitch', 'check', 'killSwitch'],
  ['setOrderValidator', 'validate', 'orderValidator'],
];

// Build a sync runner whose strategy emits one market buy on the first
// trade, with the gate under test attached and an executor watching for
// anything that gets past it.
function scenario(setter, method, impl) {
  const reg = new lrvx.SymbolRegistry();
  const sym = reg.addSymbol('test', 'BTC', 0.01);
  const signals = [];
  const submits = [];
  const emitErrors = [];
  const runner = new lrvx.Runner(reg, sig => signals.push(sig), false);

  let fired = false;
  runner.addStrategy({
    symbols: [sym],
    onTrade(_ctx, _trade, emit) {
      if (fired) { return; }
      fired = true;
      try {
        emit.marketBuy(Number(sym), 1.0);
      } catch (e) {
        emitErrors.push(e);
      }
    },
  });
  runner.setExecutor({
    submit(order) { submits.push(order); },
    cancel() {},
    capabilities() { return {}; },
  });
  runner[setter]({ [method]: impl });

  runner.start();
  runner.onTrade(Number(sym), 100, 1, true, 1000);
  runner.stop();

  return { runner, signals, submits, emitErrors };
}

function runGateScenarios() {
  for (const [setter, method, hookName] of GATES) {
    for (const [mode, impl, wanted] of [
      ['throws', () => { throw new Error(THROW_TEXT); }, THROW_TEXT],
      // A truthy non-boolean: a binding that coerced instead of checking
      // would let this through.
      ['returns a non-boolean', () => 'yes', 'boolean'],
      // Values that cannot be turned into a string for the report: the
      // report used to convert them and threw from inside the bridge.
      ['returns a Symbol', () => Symbol('allow'), 'boolean'],
      ['returns an object whose toString throws',
       () => ({ toString() { throw new Error('no string'); } }), 'boolean'],
    ]) {
      checkGateFailure(setter, method, hookName, mode, impl, wanted);
    }
  }
}

function checkGateFailure(setter, method, hookName, mode, impl, wanted) {
  const label = `${hookName}.${method} that ${mode}`;
  let s;
  try {
    s = scenario(setter, method, impl);
  } catch (e) {
    check(false, `${label}: the run itself threw (${e.constructor.name}: ${e.message})`);
    return;
  }

  check(s.emitErrors.length === 0,
        `${label}: does not escape through C into emit.marketBuy()` +
        (s.emitErrors.length ? ` (got ${s.emitErrors[0].constructor.name}: ${s.emitErrors[0].message})` : ''));
  check(s.signals.length === 0, `${label}: denies -- no signal reaches onSignal (got ${s.signals.length})`);
  check(s.submits.length === 0, `${label}: denies -- no order reaches the executor (got ${s.submits.length})`);

  check(typeof s.runner.hookErrors === 'function',
        `${label}: Runner exposes hookErrors()`);
  if (typeof s.runner.hookErrors !== 'function') { return; }

  const errors = s.runner.hookErrors();
  check(Array.isArray(errors), `${label}: hookErrors() returns an array (got ${typeof errors})`);
  if (!Array.isArray(errors)) { return; }
  check(errors.length === 1, `${label}: exactly one failure recorded (got ${errors.length})`);
  if (errors.length === 0) { return; }

  const record = errors[errors.length - 1];
  check(record.hook === hookName, `${label}: record names the hook (got ${JSON.stringify(record.hook)})`);
  check(record.method === method, `${label}: record names the method (got ${JSON.stringify(record.method)})`);
  check(typeof record.message === 'string' && record.message.length > 0,
        `${label}: record carries a message (got ${JSON.stringify(record.message)})`);
  check(typeof record.message === 'string' && record.message.toLowerCase().includes(wanted.toLowerCase()),
        `${label}: message explains the failure (wanted "${wanted}", got ${JSON.stringify(record.message)})`);
}

if (process.argv[2] === '--child') {
  // The child exists only to prove the process survives; its checks are
  // the parent's business. Run the same scenarios and exit cleanly.
  try { runGateScenarios(); } catch (_) { /* a failed check is not an abort */ }
  console.log('child-survived');
  process.exit(0);
}

console.log('=== A failing gate denies, reports, and stays inside the binding ===');
runGateScenarios();

console.log('\n=== A gate that works is untouched by the policy ===');
for (const [setter, method, hookName] of GATES) {
  {
    const s = scenario(setter, method, () => true);
    check(s.signals.length === 1, `${hookName}.${method} returning true allows (signals=${s.signals.length})`);
    check(s.submits.length === 1, `${hookName}.${method} returning true reaches the executor (submits=${s.submits.length})`);
    if (typeof s.runner.hookErrors === 'function') {
      check(s.runner.hookErrors().length === 0,
            `${hookName}.${method} returning true records no failure`);
    }
  }
  {
    const s = scenario(setter, method, () => false);
    check(s.signals.length === 0, `${hookName}.${method} returning false denies (signals=${s.signals.length})`);
    if (typeof s.runner.hookErrors === 'function') {
      check(s.runner.hookErrors().length === 0,
            `${hookName}.${method} returning false records no failure -- a deny is a decision`);
    }
  }
}

console.log('\n=== The process survives a throwing gate ===');
{
  const child = spawnSync(process.execPath, [__filename, '--child'], { encoding: 'utf8' });
  check(child.signal === null,
        `the child was not killed by a signal (got ${child.signal})`);
  check(child.status === 0, `the child exited 0 (got ${child.status})`);
  check(String(child.stdout).includes('child-survived'),
        'the child ran past every throwing gate');
  check(!/terminate called|Assertion failed|Segmentation fault|std::terminate/i.test(String(child.stderr)),
        `the child did not abort (stderr was ${JSON.stringify(String(child.stderr).slice(0, 200))})`);
}

console.log('\n=== index.d.ts declares the reporting surface ===');
{
  const dts = fs.readFileSync(path.join(__dirname, '..', 'index.d.ts'), 'utf8');
  const runner = dts.match(/export class Runner\s*\{([\s\S]*?)\n\}/);
  check(runner !== null, 'index.d.ts declares class Runner');
  if (runner !== null) {
    check(/\bhookErrors\s*\(/.test(runner[1]), 'Runner declares hookErrors()');
  }
}

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed === 0 ? 0 : 1);
