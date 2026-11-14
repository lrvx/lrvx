'use strict';
/**
 * node/test/test_bar_ring_capacity.js -- the closed-bar ring behind
 * emit.lastClosedBar / emit.lastNClosedBars and emit.setBarRingCapacity.
 *
 *   - Once the capacity is set to k the ring never holds more than k bars:
 *     a read right after lowering it returns the newest k, oldest first, and
 *     it stays at k after the next bar. Raising it keeps the bars held.
 *   - A barType outside 0..6 (Time .. BpsRange) is a TypeError naming
 *     "<method>: barType", as a string bar type already is. 256 used to be
 *     cut to 8 bits and read the Time ring.
 *
 * Run from repo root:
 *   cd node && node test/test_bar_ring_capacity.js
 */

const path = require('path');
const lrvx = require(path.join(__dirname, '..'));

let passed = 0;
let failed = 0;
function check(cond, msg) {
  if (cond) { passed++; console.log(`  ok  ${msg}`); }
  else { failed++; console.error(`  FAIL  ${msg}`); }
}
const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);

const MIN = 60000000000n;

// A synchronous runner whose strategy hands its emitter out on the first
// bar. Bar i closes at base + i.
function rig() {
  const reg = new lrvx.SymbolRegistry();
  const sym = reg.addSymbol('test', 'RING', 0.01);
  const runner = new lrvx.Runner(reg, () => {});
  const r = {emit: null, sym};
  runner.addStrategy({
    symbols: [sym],
    onBar(_ctx, _bar, emit) { r.emit = emit; },
  });
  runner.start();
  r.push = (i, base) => {
    const c = base + i;
    runner.onBar(sym, {open: c - 0.5, high: c + 1, low: c - 1, close: c, volume: 10,
                       startTimeNs: BigInt(i) * MIN, endTimeNs: BigInt(i + 1) * MIN});
  };
  r.closes = () => r.emit.lastNClosedBars(sym, 0, 0, 100).map((b) => b.close);
  r.stop = () => runner.stop();
  return r;
}

console.log('=== lowering the capacity trims a full ring ===');
{
  const r = rig();
  for (let i = 0; i < 70; ++i) r.push(i, 1000.25);
  check(r.closes().length === 64, `the default ring holds 64 bars (${r.closes().length})`);
  r.emit.setBarRingCapacity(3);
  const now = r.closes();
  check(same(now, [1067.25, 1068.25, 1069.25]),
        `right after setBarRingCapacity(3) the newest 3 are read, oldest first ` +
        `(${now.length} bars: ${now.slice(0, 3)}...)`);
  const last = r.emit.lastClosedBar(r.sym, 0, 0);
  check(last !== null && last.close === 1069.25, 'lastClosedBar is still the newest bar');
  r.push(70, 1000.25);
  const after = r.closes();
  check(same(after, [1068.25, 1069.25, 1070.25]),
        `one bar later the ring still holds 3 (${after.length} bars: ${after.slice(0, 3)}...)`);
  r.stop();
}

console.log('=== raising the capacity keeps the bars held ===');
{
  const r = rig();
  r.push(0, 2000.5);
  r.emit.setBarRingCapacity(3);
  for (let i = 1; i < 5; ++i) r.push(i, 2000.5);
  r.emit.setBarRingCapacity(5);
  check(same(r.closes(), [2002.5, 2003.5, 2004.5]), `raising to 5 keeps the 3 held (${r.closes()})`);
  r.push(5, 2000.5);
  r.push(6, 2000.5);
  check(same(r.closes(), [2002.5, 2003.5, 2004.5, 2005.5, 2006.5]),
        `the ring grows to 5 (${r.closes()})`);
  r.push(7, 2000.5);
  check(same(r.closes(), [2003.5, 2004.5, 2005.5, 2006.5, 2007.5]),
        `and then evicts the oldest (${r.closes()})`);
  r.stop();
}

console.log('=== a bar type outside 0..6 is refused ===');
{
  const r = rig();
  r.push(0, 3000);
  for (const [label, bt] of [['7', 7], ['255', 255], ['256 (wraps to Time)', 256]]) {
    for (const [method, call] of [
      ['lastClosedBar', () => r.emit.lastClosedBar(r.sym, bt, 0)],
      ['lastNClosedBars', () => r.emit.lastNClosedBars(r.sym, bt, 0, 5)],
    ]) {
      let outcome;
      try {
        const got = call();
        outcome = `no throw, returned ${JSON.stringify(got, (_k, v) =>
          typeof v === 'bigint' ? String(v) : v)}`;
      } catch (e) {
        outcome = e instanceof TypeError && String(e.message).includes(`${method}: barType`)
          ? 'TypeError' : `${e.constructor.name}: ${e.message}`;
      }
      check(outcome === 'TypeError', `${method} refuses barType ${label} (${outcome})`);
    }
  }
  const ok = r.emit.lastClosedBar(r.sym, 6, 0) === null &&
             r.emit.lastClosedBar(r.sym, 0, 0).close === 3000;
  check(ok, 'barType 6 and 0 are still read');
  r.stop();
}

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed === 0 ? 0 : 1);
