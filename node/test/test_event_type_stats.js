'use strict';
// EventTypeStatsAggregator rows through the Node binding. The row object is
// built field by field from the C row, so a column the struct grew but the
// binding did not would be missing here; the test pins the whole row with a
// distinct count per column.

const assert = require('node:assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const lrvx = require(path.join(__dirname, '..'));

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'lrvx-ets-'));
const level = (price) => BigInt64Array.from([BigInt(Math.round(price * 1e8)), 100000000n]);

try {
  const w = new lrvx.DataWriter(dir, 4, 0);
  let ts = 1700000000000000000n;
  for (let i = 0; i < 3; i++) {
    ts += 1n;
    w.writeTrade(ts, ts, 100.0, 1.0, i, 7, 0);
  }
  for (let i = 0; i < 2; i++) {
    ts += 1n;
    w.writeBook(ts, ts, BigInt(i), 7, true, level(99.0), level(101.0));
  }
  for (let i = 0; i < 5; i++) {
    ts += 1n;
    w.writeBook(ts, ts, BigInt(10 + i), 7, false, level(99.5), level(100.5));
  }
  for (let i = 0; i < 4; i++) {
    ts += 1n;
    w.writeBook(ts, ts, BigInt(20 + i), 7, false, level(99.7), level(100.3), 2);
  }
  // updateType is the record byte (0, 1, 2); isSnapshot true with a
  // non-snapshot updateType is a contradiction.
  assert.throws(() => w.writeBook(ts, ts, 0n, 7, false, level(99.7), level(100.3), 3), RangeError);
  // Not truncated: 1.9 is no delta and NaN is no snapshot.
  assert.throws(() => w.writeBook(ts, ts, 0n, 7, false, level(99.7), level(100.3), 1.9), RangeError);
  assert.throws(() => w.writeBook(ts, ts, 0n, 7, false, level(99.7), level(100.3), NaN), RangeError);
  assert.throws(() => w.writeBook(ts, ts, 0n, 7, true, level(99.7), level(100.3), 2), RangeError);
  w.close();

  const agg = new lrvx.EventTypeStatsAggregator();
  assert.ok(new lrvx.DataReader(dir).run([agg], 1));
  assert.deepStrictEqual(agg.result(), [
    { symbolId: 7, trades: 3n, bookSnapshots: 2n, bookDeltas: 5n, bookTops: 4n },
  ]);
  console.log('test_event_type_stats OK');
} finally {
  fs.rmSync(dir, { recursive: true, force: true });
}
