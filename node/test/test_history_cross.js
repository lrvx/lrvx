'use strict';
/**
 * node/test/test_history_cross.js -- History and crossedAbove / crossedBelow.
 *
 * History(capacity) keeps the last `capacity` values pushed; get(0) is the
 * newest, get(1) the one before. crossedAbove(aPrev, a, bPrev, b) is
 * aPrev <= bPrev && a > b; crossedBelow is aPrev >= bPrev && a < b. A fixed
 * level goes in as both b values. Any null or NaN input answers false.
 *
 * Run from repo root:
 *   cd node && node test/test_history_cross.js
 */

const path = require('path');
const lrvx = require(path.join(__dirname, '..'));

let passed = 0;
let failed = 0;
function check(cond, msg) {
  if (cond) { passed++; console.log(`  ok  ${msg}`); }
  else { failed++; console.error(`  FAIL  ${msg}`); }
}
function throws(fn, type, msg) {
  try {
    fn();
    check(false, `${msg} (did not throw)`);
  } catch (e) {
    check(e instanceof type, `${msg} (${e && e.name}: ${e && e.message})`);
  }
}
function section(name, body) {
  console.log(`=== ${name} ===`);
  try {
    body();
  } catch (e) {
    check(false, `${name}: threw ${e && e.name}: ${e && e.message}`);
  }
}

section('History: newest first, bounded by capacity', () => {
  throws(() => new lrvx.History(0), RangeError, 'capacity 0 rejected');
  throws(() => new lrvx.History(-2), RangeError, 'negative capacity rejected');
  const h = new lrvx.History(3);
  check(h.capacity === 3, `capacity ${h.capacity}`);
  check(h.size === 0 && h.full === false, 'empty');
  throws(() => h.get(0), RangeError, 'get(0) on an empty History');
  h.push(10.5);
  check(h.size === 1 && h.get(0) === 10.5 && h.full === false, 'one value');
  throws(() => h.get(1), RangeError, 'index == size');
  h.push(20.25);
  check(h.get(0) === 20.25 && h.get(1) === 10.5, `0 is newest, 1 previous (${h.get(0)}, ${h.get(1)})`);
  h.push(30.125);
  check(h.size === 3 && h.full === true, 'full at capacity');
  check(h.get(2) === 10.5, `oldest at 2 (${h.get(2)})`);
  h.push(40.0);
  check(h.size === 3, `size stays at capacity (${h.size})`);
  check(h.get(0) === 40.0 && h.get(1) === 30.125 && h.get(2) === 20.25,
        `the oldest dropped out (${h.get(0)}, ${h.get(1)}, ${h.get(2)})`);
  throws(() => h.get(3), RangeError, 'index == capacity');
  h.push(NaN);
  check(Number.isNaN(h.get(0)) && h.get(1) === 40.0, 'NaN is stored as given');
  const one = new lrvx.History(1);
  one.push(1); one.push(2);
  check(one.size === 1 && one.get(0) === 2 && one.full === true, 'capacity 1 keeps the newest');
});

section('crossedAbove / crossedBelow', () => {
  const up = lrvx.crossedAbove;
  const down = lrvx.crossedBelow;
  check(typeof up === 'function' && typeof down === 'function', 'both exported');
  check(up(1, 3, 2, 2) === true, 'from below to above a fixed level');
  check(up(2, 3, 2, 2) === true, 'leaving from an equal start is a cross');
  check(up(1, 2, 2, 2) === false, 'touching without crossing is not a cross');
  check(up(3, 4, 2, 2) === false, 'already above is not a cross');
  check(up(3, 1, 2, 2) === false, 'a fall is not a cross above');
  check(up(1, 3, 2, 2.5) === true, 'against a moving line');
  check(up(1, 3, 0.5, 3.5) === false, 'the line moved up faster');
  check(up(1, 3, 2, 3) === false, 'a ends equal to b: not above');
  check(down(3, 1, 2, 2) === true, 'from above to below');
  check(down(2, 1, 2, 2) === true, 'leaving an equal start downward is a cross');
  check(down(3, 2, 2, 2) === false, 'touching from above is not a cross');
  check(down(1, 0, 2, 2) === false, 'already below is not a cross');
  check(down(1, 3, 2, 2) === false, 'a rise is not a cross below');
  for (const args of [[NaN, 3, 2, 2], [1, NaN, 2, 2], [1, 3, NaN, 2], [1, 3, 2, NaN],
                      [null, 3, 2, 2], [1, null, 2, 2], [1, 3, null, 2], [1, 3, 2, null]]) {
    check(up(...args) === false, `crossedAbove(${args}) is false`);
  }
  for (const args of [[NaN, 1, 2, 2], [3, NaN, 2, 2], [3, 1, NaN, 2], [3, 1, 2, NaN],
                      [null, 1, 2, 2], [3, null, 2, 2], [3, 1, null, 2], [3, 1, 2, null]]) {
    check(down(...args) === false, `crossedBelow(${args}) is false`);
  }
});

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed > 0 ? 1 : 0);
