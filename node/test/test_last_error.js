'use strict';
/**
 * node/test/test_last_error.js -- lastError() says why a call that returns
 * false or 0 failed.
 *
 * FundingSchedule.loadTape, recompress, extractSymbols and extractTimeRange
 * report failure through their return value, and 0 is also what an extract
 * that matches nothing returns.
 *
 * Run from repo root:
 *   cd node && node test/test_last_error.js
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

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'lrvx-last-error-'));
const missing = path.join(root, 'no_such_file');
const out = path.join(root, 'out.lrvx');

console.log('=== a failure leaves its reason ===');
{
  check(typeof lrvx.lastError() === 'string', 'lastError() is a string');

  const schedule = new lrvx.FundingSchedule();
  check(schedule.loadTape(missing) === false, 'loadTape returns false for a missing file');
  check(lrvx.lastError().includes(missing), `and lastError() names it: ${lrvx.lastError()}`);

  check(lrvx.recompress(missing, out) === false, 'recompress returns false');
  check(lrvx.lastError().includes(missing) && lrvx.lastError().includes('lrvx_segment_recompress'),
        `and lastError() names the input and the function: ${lrvx.lastError()}`);

  check(lrvx.extractSymbols(missing, out, new Uint32Array([1])) === 0, 'extractSymbols returns 0');
  check(lrvx.lastError().includes(missing), 'and lastError() names the input');

  check(lrvx.extractTimeRange(missing, out, 0n, 1000000000n) === 0, 'extractTimeRange returns 0');
  check(lrvx.lastError().includes(missing), 'and lastError() names the input');
}

console.log('=== a call that did not fail leaves none ===');
{
  const dir = path.join(root, 'tape');
  const w = new lrvx.DataWriter(dir, 16, 1);
  for (let i = 0; i < 5; i++) {
    const ts = BigInt(10 + i) * 1000000000n;
    w.writeTrade(ts, ts, 100 + i, 1, i + 1, 1, 'buy');
  }
  w.close();
  const segment = path.join(dir, fs.readdirSync(dir).find((f) => f.endsWith('.lrvx')));

  // Leave an error behind first: the calls below must clear it.
  lrvx.recompress(missing, out);
  check(lrvx.extractSymbols(segment, path.join(root, 'none.lrvx'), new Uint32Array([99])) === 0,
        'an extract that matches nothing returns 0');
  check(lrvx.lastError() === '', `with no error ('${lrvx.lastError()}')`);

  lrvx.recompress(missing, out);
  check(lrvx.extractTimeRange(segment, path.join(root, 'none2.lrvx'), 500n * 1000000000n,
                              600n * 1000000000n) === 0,
        'a window past the data returns 0');
  check(lrvx.lastError() === '', 'with no error');

  check(lrvx.extractSymbols(segment, path.join(root, 'all.lrvx'), new Uint32Array([1])) === 5,
        'a matching extract counts its events');
  lrvx.extractSymbols(missing, out, new Uint32Array([1]));
  check(lrvx.recompress(segment, path.join(root, 'lz4.lrvx')) === true, 'recompress succeeds');
  check(lrvx.lastError() === '', 'with no error');

  const tape = path.join(root, 'funding.csv');
  fs.writeFileSync(tape, 'timestamp_ns,symbol,funding_rate\n1000,1,0.0001\n');
  const schedule = new lrvx.FundingSchedule();
  schedule.loadTape(missing);
  check(schedule.loadTape(tape) === true, 'loadTape reads a file that is there');
  check(lrvx.lastError() === '', 'with no error');
}

fs.rmSync(root, {recursive: true, force: true});

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed > 0 ? 1 : 0);
