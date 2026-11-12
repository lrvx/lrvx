// node/test/test_reader_undecodable.js
//
// DataReader stats().segmentsTruncated: a frame the reader cannot decode (here
// an uncompressed frame given a length above the 10 MiB limit) ends its
// segment; the events before it are delivered, the read goes on with the next
// segment, and the segment is counted.

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
const SEGMENT_HEADER = 64;
const TRADE_FRAME = 12 + 48;

function writeSegment(dir, first, n)
{
  const w = new lrvx.DataWriter(dir, 64, 0);
  for (let i = 0; i < n; ++i) {
    const ts = first + BigInt(i) * MS;
    w.writeTrade(ts, ts, 100.0 + i, 1.0, i, 1, 0);
  }
  w.close();
}

function main()
{
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'lrvx-undecodable-'));
  try {
    writeSegment(dir, BASE, 100);
    writeSegment(dir, BASE + 1000n * MS, 100);
    writeSegment(dir, BASE + 2000n * MS, 100);

    {
      const r = new lrvx.DataReader(dir);
      check(r.readTrades().length === 300, 'clean tape returns 300 trades');
      check(r.stats().segmentsTruncated === 0, 'clean tape: segmentsTruncated === 0');
    }

    const segments = fs.readdirSync(dir).filter((f) => f.endsWith('.lrvx')).sort();
    check(segments.length === 3, 'three segments');
    const fd = fs.openSync(path.join(dir, segments[1]), 'r+');
    const size = Buffer.alloc(4);
    size.writeUInt32LE(11 * 1024 * 1024);
    fs.writeSync(fd, size, 0, 4, SEGMENT_HEADER + 40 * TRADE_FRAME);
    fs.closeSync(fd);

    {
      const r = new lrvx.DataReader(dir);
      const trades = r.readTrades();
      check(trades.length === 240, `damaged tape returns 240 trades (got ${trades.length})`);
      const s = r.stats();
      // readTrades walks the tape twice (a count, then the copy) and the
      // counters accumulate over the reader's lifetime: once per walk.
      check(s.segmentsTruncated === 2, `segmentsTruncated === 2 (got ${s.segmentsTruncated})`);
      check(s.crcErrors === 0, `crcErrors === 0 (got ${s.crcErrors})`);
    }
  } finally {
    fs.rmSync(dir, {recursive: true, force: true});
  }

  console.log(`\n${_passed} passed, ${_failed} failed`);
  process.exit(_failed === 0 ? 0 : 1);
}

main();
