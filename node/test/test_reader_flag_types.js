'use strict';
/**
 * node/test/test_reader_flag_types.js
 *
 * DataReader's strictOrdering, orderSegmentsByTime and skipSegmentsByTime are
 * booleans. They were read with ToBoolean, so the string 'false' (or any
 * non-empty string, or 1) turned a flag on. Each now takes a boolean and
 * throws a TypeError naming the option for anything else; a flag left out or
 * set to undefined keeps its default.
 *
 * Run from repo root:
 *   cd node && node test/test_reader_flag_types.js
 */

const fs = require('fs');
const os = require('os');
const path = require('path');
const lrvx = require(path.join(__dirname, '..'));

let passed = 0;
let failed = 0;
function check(cond, msg) {
  if (cond) { passed++; console.log(`  ok  ${msg}`); }
  else { failed++; console.error(`  FAIL  ${msg}`); }
}

const SEC = 1_000_000_000n;
const BASE = 1_700_000_000n * SEC;

// A trade on symbol 2 stamped 40 s behind the trades around it.
function writeLaggingTape(dir) {
  const w = new lrvx.DataWriter(dir, 4, 0);
  for (let i = 0; i < 20; ++i) {
    const ts = BASE + BigInt(i) * SEC;
    w.writeTrade(ts, ts, 100.0, 1.0, i, 1, 0);
  }
  const late = BASE - 30n * SEC;
  w.writeTrade(late, late, 100.0, 1.0, 999, 2, 0);
  for (let i = 20; i < 40; ++i) {
    const ts = BASE + BigInt(i) * SEC;
    w.writeTrade(ts, ts, 100.0, 1.0, i, 1, 0);
  }
  w.close();
}

// Whether a run over the tape stops on the late event (strict ordering).
function stops(dir, opts) {
  const r = new lrvx.DataReader(dir, undefined, undefined, opts);
  return r.run([new lrvx.OHLCBinAggregator(60_000_000_000n, false)], 1) === false;
}

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'lrvx-flags-'));
try {
  writeLaggingTape(dir);

  check(stops(dir, { strictOrdering: true }), 'strictOrdering: true stops on the late event');
  check(!stops(dir, { strictOrdering: false }), 'strictOrdering: false drops it and goes on');
  check(!stops(dir, { strictOrdering: undefined }), 'strictOrdering: undefined keeps the default');
  check(!stops(dir, {}), 'no strictOrdering keeps the default');

  for (const key of ['strictOrdering', 'orderSegmentsByTime', 'skipSegmentsByTime']) {
    for (const [what, value] of [["the string 'false'", 'false'], ['1', 1], ['null', null]]) {
      try {
        new lrvx.DataReader(dir, undefined, undefined, { [key]: value });
        check(false, `${key}: ${what} is refused: nothing was thrown`);
      } catch (e) {
        check(e instanceof TypeError && e.message === `DataReader: ${key} must be a boolean`,
              `${key}: ${what} is refused (${e.constructor.name}: ${e.message})`);
      }
    }
    try {
      new lrvx.DataReader(dir, undefined, undefined, { [key]: false });
      new lrvx.DataReader(dir, undefined, undefined, { [key]: undefined });
      check(true, `${key} takes a boolean and undefined`);
    } catch (e) {
      check(false, `${key} takes a boolean and undefined (${e.message})`);
    }
  }
} finally {
  fs.rmSync(dir, { recursive: true, force: true });
}

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed === 0 ? 0 : 1);
