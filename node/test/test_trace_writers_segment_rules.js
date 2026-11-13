'use strict';
/**
 * node/test/test_trace_writers_segment_rules.js
 *
 *   - TraceRecorder.writeSignal writes the whole 64-bit signal id (a Number
 *     or a BigInt); it kept the low 32 bits.
 *
 * Run from repo root:
 *   cd node && node test/test_trace_writers_segment_rules.js
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

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'lrvx-writers-'));

console.log('=== writeSignal keeps an id above 32 bits ===');
{
  const dir = path.join(root, 'wide.lrvxrun');
  const rec = new lrvx.TraceRecorder({path: dir, strategyId: 'wide'});
  rec.writeSignal({runTsNs: 1n, signalId: 7 * 4294967296 + 5, name: 'number'});
  rec.writeSignal({runTsNs: 2n, signalId: (7n << 32n) + 6n, name: 'bigint'});
  rec.writeSignal({runTsNs: 3n, signalId: 4294967295, name: 'small'});
  rec.close();

  const reader = new lrvx.TraceReader(dir);
  const sigs = reader.readAllSignals();
  reader.close();
  check(sigs[0].signalId64 === 7 * 4294967296 + 5,
        `a wide Number id reads back whole (${sigs[0].signalId64})`);
  check(sigs[0].signalId === 5, 'and its low half is signalId');
  check(sigs[1].signalId64 === 7 * 4294967296 + 6,
        `a wide BigInt id reads back whole (${sigs[1].signalId64})`);
  check(sigs[2].signalId === 4294967295 && sigs[2].signalId64 === 4294967295,
        'an id below 2^32 is unchanged');
}

fs.rmSync(root, {recursive: true, force: true});
console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed === 0 ? 0 : 1);
