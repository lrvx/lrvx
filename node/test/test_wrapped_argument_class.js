'use strict';
/**
 * node/test/test_wrapped_argument_class.js
 *
 * A method that takes an instance of an lrvx class checks the class of what
 * it is given. Each of these read any wrapped object as the class it wanted:
 * an instance of another lrvx class was taken as, say, a SimulatedExecutor and
 * its memory read as one. Now each throws a TypeError naming the class it
 * expects, for another lrvx class's instance and for a plain object alike.
 *
 * Run from repo root:
 *   cd node && node test/test_wrapped_argument_class.js
 */

const path = require('path');
const lrvx = require(path.join(__dirname, '..'));

let passed = 0;
let failed = 0;
function check(cond, msg) {
  if (cond) { passed++; console.log(`  ok  ${msg}`); }
  else { failed++; console.error(`  FAIL  ${msg}`); }
}
function refuses(fn, cls, msg) {
  try {
    fn();
    check(false, `${msg}: nothing was thrown`);
  } catch (e) {
    const article = /^[AEIOU]/.test(cls) ? 'an' : 'a';
    check(e instanceof TypeError && String(e.message).includes(`must be ${article} ${cls}`),
          `${msg} (${e.constructor.name}: ${e.message})`);
  }
}

const reg = new lrvx.SymbolRegistry();
reg.addSymbol('test', 'BTCUSDT', 0.01);
const account = new lrvx.Account(1, 1000);
const executor = new lrvx.SimulatedExecutor();
const strangers = [
  ['an Account', account],
  ['a SymbolRegistry', reg],
  ['a SimulatedExecutor', executor],
  ['a plain object', {}],
];

// [method, expected class, call taking the argument]
const cases = [
  ['Engine.run', 'SignalBuilder', (a) => new lrvx.Engine().run(a)],
  ['BacktestRunner.addJourneyTracer', 'OrderJourneyTracer',
   (a) => new lrvx.BacktestRunner(reg, 0, 10000).addJourneyTracer(a)],
  ['Runner.attachTraceRecorder', 'TraceRecorder',
   (a) => new lrvx.Runner(reg, () => {}, false).attachTraceRecorder(a)],
  ['SimulatedExecutor.setSubmitAckLatencyDistribution', 'LatencyDistribution',
   (a) => new lrvx.SimulatedExecutor().setSubmitAckLatencyDistribution(a)],
  ['SimulatedExecutor.setCancelAckLatencyDistribution', 'LatencyDistribution',
   (a) => new lrvx.SimulatedExecutor().setCancelAckLatencyDistribution(a)],
  ['SimulatedExecutor.setReplaceAckLatencyDistribution', 'LatencyDistribution',
   (a) => new lrvx.SimulatedExecutor().setReplaceAckLatencyDistribution(a)],
  ['SimulatedExecutor.setRateLimitPolicy', 'RateLimitPolicy',
   (a) => new lrvx.SimulatedExecutor().setRateLimitPolicy(a)],
  ['SimulatedExecutor.setVenueAvailability', 'VenueAvailability',
   (a) => new lrvx.SimulatedExecutor().setVenueAvailability(a)],
  ['BacktestResult.ingestExecutor', 'SimulatedExecutor',
   (a) => new lrvx.BacktestResult(10000, 0).ingestExecutor(a)],
  ['FeeSchedule.bindAccount', 'Account', (a) => new lrvx.FeeSchedule().bindAccount(a)],
  ['LiquidationEngine.setExecutor', 'SimulatedExecutor',
   (a) => new lrvx.LiquidationEngine().setExecutor(a)],
  ['LiquidationEngine.attachAccount', 'Account',
   (a) => new lrvx.LiquidationEngine().attachAccount(a)],
  ['WalkForwardRunner constructor', 'SymbolRegistry',
   (a) => new lrvx.WalkForwardRunner(a, 0, 10000, { testSize: 10 })],
];

for (const [method, cls, call] of cases) {
  console.log(`=== ${method} ===`);
  for (const [what, arg] of strangers) {
    if (arg.constructor && arg.constructor.name === cls) {
      continue;
    }
    refuses(() => call(arg), cls, `${method} refuses ${what}`);
  }
}

console.log('=== the right class is still taken ===');
{
  const ex = new lrvx.SimulatedExecutor();
  ex.setSubmitAckLatencyDistribution(new lrvx.LatencyDistribution());
  ex.setRateLimitPolicy(new lrvx.RateLimitPolicy());
  ex.setVenueAvailability(new lrvx.VenueAvailability());
  ex.setVenueAvailability(null);
  new lrvx.BacktestResult(10000, 0).ingestExecutor(ex);
  const fs = new lrvx.FeeSchedule();
  fs.bindAccount(account);
  fs.bindAccount(null);
  const liq = new lrvx.LiquidationEngine();
  liq.setExecutor(ex);
  liq.attachAccount(account);
  new lrvx.BacktestRunner(reg, 0, 10000).addJourneyTracer(new lrvx.OrderJourneyTracer());
  const eng = new lrvx.Engine();
  eng.run(new lrvx.SignalBuilder());
  check(true, 'instances of the expected classes are accepted');

  class MyExecutor extends lrvx.SimulatedExecutor {}
  new lrvx.LiquidationEngine().setExecutor(new MyExecutor());
  check(true, 'an instance of a subclass is accepted');
}

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed === 0 ? 0 : 1);
