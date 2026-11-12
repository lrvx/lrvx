'use strict';
/**
 * node/test/test_funding_tick.js -- FundingSchedule.tick returns the
 * settlements it found, and the clock readings it and Account hand back are
 * BigInt.
 *
 * tick() asked the C API for a count and then for the events. The count call
 * settled them, so the second call found nothing and every event came back
 * as zeros.
 *
 * The FUNDING lines are the payments with every float printed as its IEEE-754
 * bits: python/tests/test_funding_tick_across_bindings.py reads them and
 * compares them with what the Python binding returns for the same schedule.
 *
 * Run from repo root:
 *   cd node && node test/test_funding_tick.js
 */

const path = require('path');
const lrvx = require(path.join(__dirname, '..'));

let passed = 0;
let failed = 0;
function check(cond, msg) {
  if (cond) { passed++; console.log(`  ok  ${msg}`); }
  else { failed++; console.error(`  FAIL  ${msg}`); }
}
function bits(x) {
  return new BigInt64Array(new Float64Array([x]).buffer)[0].toString();
}

const T1 = 1700000000123456789n;
const T2 = 1700000028800000001n;

console.log('=== one tick returns the payments due ===');
{
  const fs = new lrvx.FundingSchedule();
  fs.setTapeBySymbol([T1, T1, T2], [1, 2, 0], [0.0001, -0.0002, 0.0003]);
  const events = fs.tick(T2, [1, 2], [2.0, -3.0], [50000.0, 2000.0]);
  for (const e of events) {
    console.log(`FUNDING ${e.timestampNs} ${e.symbol} ${bits(e.rate)} ${bits(e.markPrice)} ` +
                `${bits(e.positionSigned)} ${bits(e.amount)}`);
  }
  check(events.length === 4, `four payments (${events.length})`);
  if (events.length === 4) {
    check(typeof events[0].timestampNs === 'bigint',
          `timestampNs is a bigint (${typeof events[0].timestampNs})`);
    check(events[0].timestampNs === T1, `the first timestamp is exact (${events[0].timestampNs})`);
    check(events[3].timestampNs === T2, `the last timestamp is exact (${events[3].timestampNs})`);
    check(events[0].symbol === 1 && events[1].symbol === 2, 'symbols in the order given');
    check(events[0].rate === 0.0001, `rate of symbol 1 at the first settlement (${events[0].rate})`);
    check(events[1].rate === -0.0002, `rate of symbol 2 at the first settlement (${events[1].rate})`);
    check(events[2].rate === 0.0003 && events[3].rate === 0.0003, 'the wildcard rate at the second');
    check(events[0].markPrice === 50000 && events[0].positionSigned === 2,
          'mark and position are the ones passed in');
    check(events[0].amount === -2.0 * 50000.0 * 0.0001,
          `a long pays a positive rate (${events[0].amount})`);
    check(events[1].amount === 3.0 * 2000.0 * -0.0002,
          `a short pays a negative rate (${events[1].amount})`);
  }
  check(fs.tick(T2, [1, 2], [2.0, -3.0], [50000.0, 2000.0]).length === 0,
        'the same instant again settles nothing');
}

console.log('=== more payments than the first call has room for ===');
{
  const HOUR = 3600n * 1000000000n;
  const fs = new lrvx.FundingSchedule();
  fs.setConstant(Number(HOUR), 0.001);
  check(fs.tick(1000n * HOUR, [1], [1], [100]).length === 1,
        'the opening tick settles the boundary just passed');
  const burst = fs.tick(1040n * HOUR, [1], [1], [100]);
  check(burst.length === 40, `forty boundaries in one tick (${burst.length})`);
  if (burst.length === 40) {
    check(burst[0].timestampNs === 1001n * HOUR, 'starting after the last one settled');
    check(burst[39].timestampNs === 1040n * HOUR, 'and ending at now');
    check(burst[39].amount === -0.1, `each with its amount (${burst[39].amount})`);
  }
}

console.log('=== Account.markTsFor is the exact reading ===');
{
  const a = new lrvx.Account(1, 1000);
  a.openPosition(1, 1, 100);
  a.setMark(1, 100, T1);
  const ts = a.markTsFor(1);
  check(typeof ts === 'bigint', `markTsFor returns a bigint (${typeof ts})`);
  check(ts === T1, `and the nanosecond it was given (${ts})`);
  check(typeof a.markTsFor(7) === 'bigint' && a.markTsFor(7) < 0n,
        'a symbol never marked reads very negative');
}

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed > 0 ? 1 : 0);
