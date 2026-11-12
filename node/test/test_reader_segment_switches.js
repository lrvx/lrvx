// node/test/test_reader_segment_switches.js
//
// DataReader's two segment switches and the segment counters in stats():
//   * orderSegmentsByTime (default true) merges Sorted segments whose ranges
//     overlap; false reads them in file-name order
//   * skipSegmentsByTime (default true) leaves out the segments whose proven
//     bounds lie outside [fromNs, toNs]; false decodes every segment and
//     delivers the same events
//   * stats() carries segmentsSkipped, segmentsReordered, segmentsInterleaved
//   * run() with its default thread count delivers the overlapping segments
//     in time order (the sliding-window aggregators see no step back)

'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');
const lrvx = require('..');

let _failed = 0;
let _passed = 0;
function check(cond, msg)
{
  if (cond)
  {
    _passed++;
    console.log(`  ok  - ${msg}`);
  }
  else
  {
    _failed++;
    console.log(`  FAIL - ${msg}`);
  }
}

const MS = 1_000_000n;
const BASE = 1_700_000_000_000_000_000n;

// One writer per segment: files are named by the time the writer opened
// them, so they list in the order written.
function writeSegment(dir, first, n, step)
{
  const w = new lrvx.DataWriter(dir, 64, 0);
  for (let i = 0; i < n; ++i) {
    const ts = first + BigInt(i) * step;
    w.writeTrade(ts, ts, 100.0 + i, 1.0, i, 1, 0);
  }
  w.close();
}

function stamps(trades)
{
  return trades.map((t) => t.exchangeTsNs);
}

function isSorted(ts)
{
  for (let i = 1; i < ts.length; ++i) {
    if (ts[i] < ts[i - 1]) return false;
  }
  return true;
}

function main()
{
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'lrvx-segswitch-'));
  try {
    // a: even milliseconds from 2; b: odd ones from 1, so it starts first
    // and overlaps a; c: after both.
    writeSegment(dir, BASE + 2n * MS, 100, 2n * MS);
    writeSegment(dir, BASE + MS, 50, 2n * MS);
    writeSegment(dir, BASE + 1000n * MS, 50, MS);

    {
      const r = new lrvx.DataReader(dir);
      const ts = stamps(r.readTrades());
      check(ts.length === 200, `default reader returns 200 trades (got ${ts.length})`);
      check(isSorted(ts), 'default reader returns them in time order');
      const s = r.stats();
      check(s.segmentsInterleaved > 0, `segmentsInterleaved > 0 (got ${s.segmentsInterleaved})`);
      check(s.segmentsReordered > 0, `segmentsReordered > 0 (got ${s.segmentsReordered})`);
      check(s.segmentsSkipped === 0, `segmentsSkipped === 0 (got ${s.segmentsSkipped})`);
    }

    {
      const r = new lrvx.DataReader(dir, undefined, undefined, {orderSegmentsByTime: false});
      const ts = stamps(r.readTrades());
      check(ts.length === 200, 'file-name order returns 200 trades');
      check(ts[0] === BASE + 2n * MS,'file-name order reads the first file first');
      const s = r.stats();
      check(s.segmentsInterleaved === 0, 'file-name order interleaves nothing');
      check(s.segmentsReordered === 0, 'file-name order reorders nothing');
    }

    {
      const from = BASE + 500n * MS;
      const skip = new lrvx.DataReader(dir, from);
      const walk = new lrvx.DataReader(dir, from, undefined, {skipSegmentsByTime: false});
      const a = stamps(skip.readTrades());
      const b = stamps(walk.readTrades());
      check(a.length === 50, `a range read returns 50 trades (got ${a.length})`);
      check(a.length === b.length && a.every((v, i) => v === b[i]),
            'skipping changes nothing delivered');
      check(skip.stats().segmentsSkipped > 0, 'segmentsSkipped > 0 with skipping on');
      check(walk.stats().segmentsSkipped === 0, 'segmentsSkipped === 0 with skipping off');
    }

    {
      // The sliding-window peak sees the merged stream on every thread count.
      const peaks = (opts) => {
        const r = new lrvx.DataReader(dir);
        const agg = new lrvx.PeakAggregator([20n * MS], 5);
        r.run([agg], opts);
        return JSON.stringify(agg.result(), (k, v) => (typeof v === 'bigint' ? v.toString() : v));
      };
      check(peaks(1) === peaks(0), 'run() default matches run(1) on overlapping segments');
    }
  } finally {
    fs.rmSync(dir, {recursive: true, force: true});
  }

  console.log(`\n${_passed} passed, ${_failed} failed`);
  process.exit(_failed === 0 ? 0 : 1);
}

main();
