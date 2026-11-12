'use strict';
/**
 * node/test/test_clock_inputs_bigint.js -- functions that take a nanosecond
 * clock reading accept a BigInt and use it to the nanosecond.
 *
 * A wall-clock nanosecond reading is near 1.76e18, past 2^53, where a JS
 * Number steps 256 ns at a time. The readings here are a few nanoseconds
 * apart, so they are one value as Numbers and different values as BigInts.
 * Durations (budgets, intervals, time-to-live) stay Numbers.
 *
 * Run from repo root:
 *   cd node && node test/test_clock_inputs_bigint.js
 */

const path = require('path');
const lrvx = require(path.join(__dirname, '..'));

let passed = 0;
let failed = 0;
function check(cond, msg) {
  if (cond) { passed++; console.log(`  ok  ${msg}`); }
  else { failed++; console.error(`  FAIL  ${msg}`); }
}
// Runs a block; a throw fails the named check instead of ending the file.
function section(name, fn) {
  console.log(`=== ${name} ===`);
  try { fn(); } catch (e) { check(false, `${name}: ${e.message}`); }
}

// Odd reading: not a multiple of the 256 ns double step.
const T0 = 1757000000123456789n;
check(Number(T0) === Number(T0 + 10n), 'the readings used here are one value as a Number');

section('VenueAvailability', () => {
  const va = new lrvx.VenueAvailability();
  va.scheduleOutage(T0 + 10n, 20, 'cancel_all');
  check(va.isUp(T0 + 9n) === true, 'isUp just before the outage');
  check(va.isUp(T0 + 10n) === false, 'isUp at the outage start');
  check(va.isUp(T0 + 29n) === false, 'isUp inside the outage');
  check(va.isUp(T0 + 30n) === true, 'isUp once the outage is over');
  check(va.submitsAllowed(T0 + 9n) === true && va.submitsAllowed(T0 + 10n) === false,
        'submitsAllowed');
  check(va.cancelsAllowed(T0 + 9n) === true && va.cancelsAllowed(T0 + 10n) === false,
        'cancelsAllowed');
  check(va.bookUpdatesAllowed(T0 + 9n) === true && va.bookUpdatesAllowed(T0 + 10n) === false,
        'bookUpdatesAllowed');
  check(va.tradesAllowed(T0 + 9n) === true && va.tradesAllowed(T0 + 10n) === false,
        'tradesAllowed');

  const slow = new lrvx.VenueAvailability();
  slow.scheduleOutageEx(T0 + 10n, 20, 'slow_degradation', 'hold', 0, 3.0);
  check(slow.latencyMultiplier(T0 + 9n) === 1 && slow.latencyMultiplier(T0 + 10n) === 3,
        `latencyMultiplier (${slow.latencyMultiplier(T0 + 9n)}, ${slow.latencyMultiplier(T0 + 10n)})`);

  const plain = new lrvx.VenueAvailability();
  plain.scheduleOutage(1000, 20);
  check(plain.isUp(999) === true && plain.isUp(1000) === false, 'a Number is still accepted');
});

section('Account marks and fills', () => {
  const a = new lrvx.Account(1, 1000);
  a.openPosition(1, 1, 100);
  a.setMark(1, 100, T0);
  check(a.hasStaleMarks(T0 + 100n, 100) === false, 'hasStaleMarks: a mark exactly at the budget');
  check(a.hasStaleMarks(T0 + 101n, 100) === true, 'hasStaleMarks: one nanosecond past it');
  a.recordFill(T0, 500, 1);
  check(a.rollingNotional30d() === 500, `recordFill counts the fill (${a.rollingNotional30d()})`);
});

section('VenueStack fee proxies', () => {
  const stack = lrvx.VenueStack.binanceUmFutures(1, 1000);
  stack.feesRecordFill(T0, 1000);
  const fee = stack.feesFor(T0 + 1n, 1000, true);
  check(typeof fee === 'number' && Number.isFinite(fee), `feesFor returns a fee (${fee})`);
});

section('LiquidationEngine.onMarks', () => {
  const liq = new lrvx.LiquidationEngine();
  const n = liq.onMarks([[1, 100]], T0);
  check(n === 0, `onMarks with a BigInt reading (${n})`);
});

section('FundingSchedule', () => {
  const fs = new lrvx.FundingSchedule();
  fs.setTape([T0 + 10n, T0 + 20n], [0.001, 0.002]);
  check(fs.tick(T0 + 9n, [1], [1], [100]).length === 0, 'tick before the first settlement');
  check(fs.tick(T0 + 10n, [1], [1], [100]).length === 1, 'tick at the first settlement');
  check(fs.tick(T0 + 19n, [1], [1], [100]).length === 0, 'tick between settlements');
  check(fs.tick(T0 + 20n, [1], [1], [100]).length === 1, 'tick at the second settlement');

  const bySymbol = new lrvx.FundingSchedule();
  bySymbol.setTapeBySymbol([T0 + 10n], [1], [0.001]);
  check(bySymbol.tick(T0 + 9n, [1], [1], [100]).length === 0 &&
        bySymbol.tick(T0 + 10n, [1], [1], [100]).length === 1, 'setTapeBySymbol');

  const plain = new lrvx.FundingSchedule();
  plain.setTape([1000], [0.001]);
  check(plain.tick(999, [1], [1], [100]).length === 0 &&
        plain.tick(1000, [1], [1], [100]).length === 1, 'Numbers are still accepted');
});

section('OrderGroup.pairLatencyDecision', () => {
  const g = new lrvx.OrderGroup();
  g.setPairLatencyBudgetNs(100);
  const at = (ackOffset, ackReceived) => g.pairLatencyDecision(
      {leaderSubmitTsNs: T0, leaderAckTsNs: T0 + ackOffset, ackReceived});
  check(at(100n, true) === 'submit_follower', `an ack exactly at the budget (${at(100n, true)})`);
  check(at(101n, true) === 'cancel_leader', `an ack one nanosecond past it (${at(101n, true)})`);
  check(at(50n, false) === 'wait', 'no ack yet');
});

section('pool tape', () => {
  const tape = lrvx.poolTapeCreate();
  lrvx.poolTapeDescriptorConstantProduct(tape, 3, 1000, 18, 6);
  lrvx.poolTapeCheckpoint(tape, T0, 1000000n, 1000000n);
  lrvx.poolTapeSwap(tape, T0 + 1n, true, 1000n);
  const replay = lrvx.poolTapeReplay(tape, 0, 1, 18, 6);
  check(lrvx.poolReplayTradeCount(replay) === 1, 'checkpoint and swap at BigInt readings');
});

section('BarDispatchRecorder.onTrade', () => {
  const r = new lrvx.BarDispatchRecorder();
  r.addTimeIntervalSeconds(1);
  const SEC = 1000000000n;
  r.onTrade(1, 100, 1, T0);
  r.onTrade(1, 101, 1, T0 + 2n * SEC);
  r.onTrade(1, 101, 1, T0 + 4n * SEC);
  r.finalize();
  check(r.count() >= 2, `bars close on BigInt readings (${r.count()})`);
});

section('SimulatedExecutor expiresAtNs', () => {
  const ex = new lrvx.SimulatedExecutor();
  ex.submitOrder(1, 'buy', 90, 1, 'limit', 1, {tif: 'gtd', expiresAtNs: T0 + 5n});
  check(true, 'submitOrder takes a BigInt expiresAtNs');
});

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed > 0 ? 1 : 0);
