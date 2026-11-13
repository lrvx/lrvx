'use strict';
/**
 * node/test/test_worker_envs.js
 *
 * Node loads the addon once per environment: the main thread and each worker
 * thread get their own module instance. The constructors the binding checks
 * arguments against were process-wide statics, so the environment that loaded
 * last overwrote them and every other environment compared its own instances
 * against a constructor from another isolate. Each environment now keeps its
 * own.
 *
 * The main thread loads the addon, two workers load it while the main thread
 * keeps using it, and each environment constructs and passes wrapped objects
 * before and after the others load and after the workers exit.
 *
 * Run from repo root:
 *   cd node && node test/test_worker_envs.js
 */

const path = require('path');
const { Worker, isMainThread, parentPort, workerData } = require('worker_threads');
const lrvx = require(path.join(__dirname, '..'));

// Constructs objects and passes them where an argument's class is checked;
// returns the failures.
function exercise() {
  const failures = [];
  const step = (name, fn) => {
    try { fn(); } catch (e) { failures.push(`${name}: ${e.message}`); }
  };
  const reg = new lrvx.SymbolRegistry();
  reg.addSymbol('test', 'BTCUSDT', 0.01);
  const account = new lrvx.Account(1, 1000);
  const ex = new lrvx.SimulatedExecutor();
  step('setSubmitAckLatencyDistribution',
       () => ex.setSubmitAckLatencyDistribution(new lrvx.LatencyDistribution()));
  step('setRateLimitPolicy', () => ex.setRateLimitPolicy(new lrvx.RateLimitPolicy()));
  step('setVenueAvailability', () => ex.setVenueAvailability(new lrvx.VenueAvailability()));
  step('ingestExecutor', () => new lrvx.BacktestResult(10000, 0).ingestExecutor(ex));
  step('bindAccount', () => new lrvx.FeeSchedule().bindAccount(account));
  step('setExecutor', () => new lrvx.LiquidationEngine().setExecutor(ex));
  step('addJourneyTracer',
       () => new lrvx.BacktestRunner(reg, 0, 10000).addJourneyTracer(new lrvx.OrderJourneyTracer()));
  step('BacktestRunner latency', () => new lrvx.BacktestRunner(
      reg, 0, 10000, { latency: new lrvx.ConstantLatency({ feedNs: 0, orderNs: 1, fillNs: 0 }) }));
  step('Engine.run', () => new lrvx.Engine().run(new lrvx.SignalBuilder()));
  step('VenueStack.binanceUmFutures', () => {
    const s = lrvx.VenueStack.binanceUmFutures(1, 1000);
    if (!(s instanceof lrvx.VenueStack)) throw new Error('not a VenueStack of this environment');
  });
  step('a foreign class is still refused', () => {
    let threw = false;
    try { new lrvx.LiquidationEngine().setExecutor(account); } catch (e) { threw = e instanceof TypeError; }
    if (!threw) throw new Error('an Account was taken as a SimulatedExecutor');
  });
  return failures;
}

if (!isMainThread) {
  const results = [exercise()];
  parentPort.postMessage({ id: workerData.id, phase: 'loaded', failures: results[0] });
  parentPort.on('message', (msg) => {
    if (msg === 'again') {
      parentPort.postMessage({ id: workerData.id, phase: 'again', failures: exercise() });
    } else if (msg === 'exit') {
      parentPort.close();
    }
  });
  return;
}

let passed = 0;
let failed = 0;
function check(failures, msg) {
  if (failures.length === 0) { passed++; console.log(`  ok  ${msg}`); }
  else { failed++; console.error(`  FAIL  ${msg}\n        ${failures.join('\n        ')}`); }
}

function waitFor(worker, phase) {
  return new Promise((resolve, reject) => {
    const onMsg = (m) => {
      if (m.phase === phase) {
        worker.off('message', onMsg);
        resolve(m);
      }
    };
    worker.on('message', onMsg);
    worker.once('error', reject);
  });
}

(async () => {
  check(exercise(), 'main thread before any worker loads');

  const workers = [1, 2].map((id) => new Worker(__filename, { workerData: { id } }));
  const loaded = await Promise.all(workers.map((w) => waitFor(w, 'loaded')));
  for (const m of loaded) {
    check(m.failures, `worker ${m.id} after it loads`);
  }
  check(exercise(), 'main thread after both workers load');

  const again = await Promise.all(workers.map((w) => {
    const p = waitFor(w, 'again');
    w.postMessage('again');
    return p;
  }));
  for (const m of again) {
    check(m.failures, `worker ${m.id} after the main thread ran again`);
  }

  await Promise.all(workers.map((w) => {
    const done = new Promise((resolve) => w.once('exit', resolve));
    w.postMessage('exit');
    return done;
  }));
  check(exercise(), 'main thread after the workers exit');

  console.log(`\n${passed} passed, ${failed} failed`);
  process.exit(failed === 0 ? 0 : 1);
})().catch((e) => {
  console.error(e);
  process.exit(1);
});
