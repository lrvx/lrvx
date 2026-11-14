'use strict';
/**
 * node/test/test_highest_lowest.js -- the streaming Highest(n) / Lowest(n).
 *
 * The window holds the last n values, the current one included. Same surface
 * and warm-up convention as SMA: update() returns the value or null while not
 * ready, value / ready / count are read-only properties, reset() starts over,
 * compute(Float64Array) runs the same steps on a fresh instance with NaN where
 * the stream reads null. A NaN poisons the window it is in, as it does SMA's.
 *
 * Run from repo root:
 *   cd node && node test/test_highest_lowest.js
 */

const path = require('path');
const lrvx = require(path.join(__dirname, '..'));

let passed = 0;
let failed = 0;
function check(cond, msg) {
  if (cond) { passed++; console.log(`  ok  ${msg}`); }
  else { failed++; console.error(`  FAIL  ${msg}`); }
}
function section(name, body) {
  console.log(`=== ${name} ===`);
  try {
    body();
  } catch (e) {
    check(false, `${name}: threw ${e && e.name}: ${e && e.message}`);
  }
}
const same = (a, b) => (Number.isNaN(a) && Number.isNaN(b)) || a === b;

// The maximum, 9, leaves the window at step 4 and the next-highest (7) then
// wins. Equal values (7, 7) and a NaN follow.
const SERIES = [5, 9, 4, 3, 7, 7, 2, 8, NaN, 1, 6, 0.5, 6];
const WANT_HIGH = [null, null, 9, 9, 7, 7, 7, 8, null, null, null, 6, 6];
const WANT_LOW = [null, null, 4, 3, 3, 3, 2, 2, null, null, null, 0.5, 0.5];

function streams(Cls, want, label) {
  const ind = new Cls(3);
  check(ind.value === null && ind.ready === false && ind.count === 0, `${label}: fresh`);
  SERIES.forEach((x, i) => {
    const r = ind.update(x);
    check(r === want[i], `${label} step ${i} (x=${x}): update -> ${r}, want ${want[i]}`);
    check(ind.value === want[i], `${label} step ${i}: value ${ind.value}`);
    check(ind.ready === (want[i] !== null), `${label} step ${i}: ready ${ind.ready}`);
    check(ind.count === i + 1, `${label} step ${i}: count ${ind.count}`);
  });
  const batch = new Cls(3).compute(new Float64Array(SERIES));
  check(batch instanceof Float64Array && batch.length === SERIES.length,
        `${label}: compute returns a Float64Array of the input length`);
  want.forEach((w, i) => {
    check(same(batch[i], w === null ? NaN : w), `${label} compute[${i}] ${batch[i]}, want ${w}`);
  });
  // Batch equals streaming element by element on a longer walk.
  const walk = new Float64Array(200);
  let px = 100;
  for (let i = 0; i < walk.length; i++) {
    px += Math.sin(i * 1.7) * 3 + (i % 7 === 0 ? -4 : 1.25);
    walk[i] = i === 50 || i === 51 || i === 120 ? NaN : px;
  }
  const s = new Cls(14);
  const b = new Cls(14).compute(walk);
  let mismatch = -1;
  for (let i = 0; i < walk.length; i++) {
    const v = s.update(walk[i]);
    if (!same(v === null ? NaN : v, b[i])) { mismatch = i; break; }
  }
  check(mismatch === -1, `${label}: streaming == compute over 200 steps (first mismatch ${mismatch})`);
  s.reset();
  check(s.value === null && s.ready === false && s.count === 0, `${label}: reset`);
  check(s.update(42) === null && s.count === 1, `${label}: warm-up restarts after reset`);
  const one = new Cls(1);
  check(one.update(3.5) === 3.5 && one.update(-1) === -1, `${label}(1) is the input`);
}

section('Highest', () => streams(lrvx.Highest, WANT_HIGH, 'Highest'));
section('Lowest', () => streams(lrvx.Lowest, WANT_LOW, 'Lowest'));

section('period 0 is rejected as SMA rejects it', () => {
  let smaCode = null;
  try { new lrvx.SMA(0); } catch (e) { smaCode = e.code; }
  for (const name of ['Highest', 'Lowest']) {
    try {
      new lrvx[name](0);
      check(false, `${name}(0) did not throw`);
    } catch (e) {
      check(e instanceof Error && e.code === smaCode,
            `${name}(0) throws with ${smaCode} (${e && e.code}: ${e && e.message})`);
    }
  }
});

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed > 0 ? 1 : 0);
