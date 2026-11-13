'use strict';
/**
 * node/test/test_trace_writers_segment_rules.js
 *
 *   - TraceRecorder.writeSignal writes the whole 64-bit signal id (a Number
 *     or a BigInt); it kept the low 32 bits.
 *   - new DataWriter(dir, 0) uses the default segment size; it wrote one
 *     segment per event.
 *   - mergeSegments returns false for a merge that skipped an input, as
 *     mergeDir's success does; it returned true.
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
  check(sigs[0].signalId64 === (7n << 32n) + 5n,
        `a wide Number id reads back whole (${sigs[0].signalId64})`);
  check(sigs[0].signalId === 5, 'and its low half is signalId');
  check(sigs[1].signalId64 === (7n << 32n) + 6n,
        `a wide BigInt id reads back whole (${sigs[1].signalId64})`);
  check(sigs[2].signalId === 4294967295 && sigs[2].signalId64 === 4294967295n,
        'an id below 2^32 is unchanged');
}

function writeTrades(writer, n) {
  for (let i = 0; i < n; i++) {
    const ts = BigInt(10 + i) * 1000000000n;
    writer.writeTrade(ts, ts, 100 + i, 1, i + 1, 1, 'buy');
  }
  writer.close();
}
const segmentsIn = (dir) => fs.readdirSync(dir).filter((f) => f.endsWith('.lrvx'));

console.log('=== a 0 MB segment size is the default ===');
{
  const zero = path.join(root, 'zero');
  writeTrades(new lrvx.DataWriter(zero, 0, 1), 5);
  check(segmentsIn(zero).length === 1,
        `new DataWriter(dir, 0, 1) writes one segment (${segmentsIn(zero).length})`);
  check(lrvx.validateDataset(zero).totalEvents === 5, 'holding the five events');

  const omitted = path.join(root, 'omitted');
  writeTrades(new lrvx.DataWriter(omitted), 5);
  check(segmentsIn(omitted).length === 1, 'as new DataWriter(dir) does');
}

console.log('=== a partial merge is not a success ===');
{
  const tape = path.join(root, 'tape');
  writeTrades(new lrvx.DataWriter(tape, 16, 1), 5);
  const whole = lrvx.mergeDir(tape, path.join(root, 'merged_whole'));
  check(whole.success === true && whole.errors.length === 0, 'every input merged: success');
  check(lrvx.mergeSegments(tape, path.join(root, 'merged_whole2')) === true,
        'and mergeSegments returns true');

  const bad = path.join(tape, 'zz_bad.lrvx');
  fs.writeFileSync(bad, Buffer.alloc(256, 'x'));
  const short = lrvx.mergeSegments(tape, path.join(root, 'merged_short'));
  check(short === false, `mergeSegments returns false when an input was skipped (${short})`);
  check(lrvx.lastError().includes(bad) && lrvx.lastError().includes('merged 1 segment(s)'),
        `and lastError() names it: ${lrvx.lastError()}`);
  check(segmentsIn(path.join(root, 'merged_short')).length === 1,
        'the inputs that opened are still written');

  const dir = lrvx.mergeDir(tape, path.join(root, 'merged_short2'));
  check(dir.success === false && dir.segmentsMerged === 1 && dir.eventsWritten === 5,
        'mergeDir says the same: success false, with the counts of what was written');
  check(dir.errors.length === 1 && dir.errors[0] === 'Cannot open: ' + bad,
        `and lists the input: ${dir.errors.join(' | ')}`);
}

fs.rmSync(root, {recursive: true, force: true});
console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed === 0 ? 0 : 1);
