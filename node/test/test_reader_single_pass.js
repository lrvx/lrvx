// node/test/test_reader_single_pass.js
//
// DataReader's array reads walk the tape once. readTrades, readBBO and
// readBookUpdates (and their *From forms) used to count in one walk and fill
// in a second, so one call doubled every stats() counter and cost two reads.
// After one call on a fresh reader the counters are what one walk of the
// tape gives: every segment opened once, every event and byte read once.

'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');
const lrvx = require('..');

let _failed = 0;
let _passed = 0;
function check(cond, msg)
{
  if (cond) {
    _passed++;
    console.log(`  ok  - ${msg}`);
  } else {
    _failed++;
    console.log(`  FAIL - ${msg}`);
  }
}

const MS = 1_000_000n;
const BASE = 1_700_000_000_000_000_000n;
const SEGMENTS = 60;
const TRADES_PER_SEGMENT = 300;
const BOOKS_PER_SEGMENT = 100;

// One segment per writer: trades with a book update after every third.
function writeSegment(dir, s)
{
  const w = new lrvx.DataWriter(dir, 64, 0);
  let k = 0;
  for (let i = 0; i < TRADES_PER_SEGMENT; ++i) {
    const ts = BASE + BigInt(s * 10_000 + k++) * MS;
    w.writeTrade(ts, ts, 100.0 + i, 1.0, s * 1000 + i, 1, i % 2);
    if (i % 3 === 2) {
      const bt = BASE + BigInt(s * 10_000 + k++) * MS;
      const bids = new BigInt64Array([10_000_000_000n, 100_000_000n, 9_900_000_000n, 200_000_000n]);
      const asks = new BigInt64Array([10_100_000_000n, 100_000_000n]);
      w.writeBook(bt, bt, BigInt(i), 2, i % 30 === 2, bids, asks);
    }
  }
  w.close();
}

// What one walk of the tape reads, from the files themselves.
function oneWalk(dir)
{
  let bytes = 0;
  const files = fs.readdirSync(dir).filter((f) => f.endsWith('.lrvx'));
  for (const f of files) {
    const buf = fs.readFileSync(path.join(dir, f));
    const indexOffset = Number(buf.readBigUInt64LE(40));
    const hasIndex = (buf[6] & 1) !== 0 && indexOffset > 0;
    bytes += (hasIndex ? indexOffset : buf.length) - 64;
  }
  return {
    filesRead: files.length,
    eventsRead: SEGMENTS * (TRADES_PER_SEGMENT + BOOKS_PER_SEGMENT),
    tradesRead: SEGMENTS * TRADES_PER_SEGMENT,
    bookUpdatesRead: SEGMENTS * BOOKS_PER_SEGMENT,
    bytesRead: bytes,
  };
}

function sameWalk(stats, want, label)
{
  for (const k of Object.keys(want)) {
    check(stats[k] === want[k], `${label}: ${k} ${stats[k]} === ${want[k]}`);
  }
}

function main()
{
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'lrvx-single-pass-'));
  try {
    for (let s = 0; s < SEGMENTS; ++s) {
      writeSegment(dir, s);
    }
    const want = oneWalk(dir);
    check(want.filesRead === SEGMENTS, `tape has ${SEGMENTS} segments`);

    // The reference: the aggregator run, one streaming walk through the C++
    // reader.
    {
      const r = new lrvx.DataReader(dir);
      const agg = new lrvx.EventTypeStatsAggregator();
      check(r.run([agg]) === true, 'run([EventTypeStats]) succeeds');
      sameWalk(r.stats(), want, 'run');
    }

    const reads = [
      ['readTrades', (r) => r.readTrades(), SEGMENTS * TRADES_PER_SEGMENT],
      ['readTradesFrom', (r) => r.readTradesFrom(BASE), SEGMENTS * TRADES_PER_SEGMENT],
      ['readBBO', (r) => r.readBBO(), SEGMENTS * BOOKS_PER_SEGMENT],
      ['readBBOFrom', (r) => r.readBBOFrom(BASE), SEGMENTS * BOOKS_PER_SEGMENT],
      ['readBookUpdates', (r) => r.readBookUpdates(), SEGMENTS * BOOKS_PER_SEGMENT],
      ['readBookUpdatesFrom', (r) => r.readBookUpdatesFrom(BASE), SEGMENTS * BOOKS_PER_SEGMENT],
    ];
    for (const [name, read, rows] of reads) {
      const r = new lrvx.DataReader(dir);
      const got = read(r);
      check(got.length === rows, `${name} returns ${rows} rows (got ${got.length})`);
      sameWalk(r.stats(), want, name);
    }

    // A second call adds exactly one more walk.
    {
      const r = new lrvx.DataReader(dir);
      r.readTrades();
      r.readTrades();
      check(r.stats().eventsRead === 2 * want.eventsRead, 'two readTrades calls: two walks');
    }

    // A limit stops the walk early and still returns what fits.
    {
      const r = new lrvx.DataReader(dir);
      const got = r.readTrades(10);
      check(got.length === 10, 'readTrades(10) returns 10 trades');
      check(got[9].tradeId === 9, 'readTrades(10) returns the first ten in order');
      check(r.stats().eventsRead < want.eventsRead, 'readTrades(10) stops the walk early');
      const bbo = new lrvx.DataReader(dir).readBBO(7);
      check(bbo.length === 7, 'readBBO(7) returns 7 rows');
    }

    // Rows match, field for field, what the reader delivers in a callback walk.
    {
      const r = new lrvx.DataReader(dir);
      const t = r.readTrades();
      check(t[0].tradeId === 0 && t[0].price === 100 && t[0].side === 'buy', 'first trade fields');
      check(t[1].side === 'sell', 'second trade is a sell');
      const b = new lrvx.DataReader(dir).readBookUpdates();
      check(b[0].bids.length === 2 && b[0].asks.length === 1, 'first book: 2 bids, 1 ask');
      check(b[0].bids[1].price === 99 && b[0].asks[0].qty === 1, 'first book levels');
      check(b[0].eventType === 0 && b[1].eventType === 1, 'snapshot then delta');
      const q = new lrvx.DataReader(dir).readBBO();
      check(q[0].bidPrice === 100 && q[0].askPrice === 101, 'first BBO');
    }
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }

  console.log(`\n${_passed} passed, ${_failed} failed`);
  if (_failed > 0) {
    process.exit(1);
  }
}

main();
