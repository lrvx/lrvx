'use strict';
// The touch on the Node SymbolContext, end to end from a tape.
//
// A TOP_OF_BOOK frame moves bestBid / bestAsk and lastTouchNs without
// firing onBookUpdate; lastUpdateNs stays on the last dispatched event.
// DataWriter.writeBook cannot write a top yet, so the fixture rewrites a
// delta frame in place: frame type 3 -> 6 at offset 8 of the FrameHeader,
// record byte 1 -> 2 at offset 32 of the BookRecordHeader, payload CRC-32
// recomputed. Frames sit back to back from byte 64 (SegmentHeader) up to
// index_offset (u64 at byte 40); see docs/spec/lrvx.md.

const assert = require('node:assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const lrvx = require(path.join(__dirname, '..'));

const SNAP_NS = 1700000000000000000n;
const TOP_NS = SNAP_NS + 1000000000n;
const TRADE_NS = TOP_NS + 1000000000n;

const raw = (x) => BigInt(Math.round(x * 1e8));
const levels = (items) => BigInt64Array.from(items.flatMap(([p, q]) => [raw(p), raw(q)]));

// CRC-32 (IEEE), the polynomial the frame header carries. Node 22 has
// zlib.crc32; the CI runner is older, so the table lives here.
const CRC_TABLE = new Uint32Array(256);
for (let n = 0; n < 256; n++) {
  let c = n;
  for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
  CRC_TABLE[n] = c >>> 0;
}
function crc32(buf) {
  let c = 0xffffffff;
  for (const b of buf) c = CRC_TABLE[(c ^ b) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

// Returns the number of frames rewritten, so a fixture that silently
// rewrote nothing fails loudly instead of testing a delta.
function rewriteLastDeltaAsTop(dir) {
  let rewritten = 0;
  for (const name of fs.readdirSync(dir).filter((n) => n.endsWith('.lrvx')).sort()) {
    const file = path.join(dir, name);
    const data = fs.readFileSync(file);
    const indexOffset = Number(data.readBigUInt64LE(40));
    const end = indexOffset || data.length;
    let pos = 64;
    let lastDelta = -1;
    while (pos + 12 <= end) {
      const size = data.readUInt32LE(pos);
      if (data[pos + 8] === 3) lastDelta = pos;
      pos += 12 + size;
    }
    if (lastDelta < 0) continue;
    const size = data.readUInt32LE(lastDelta);
    data[lastDelta + 8] = 6;         // FrameHeader.type: BookTop
    data[lastDelta + 12 + 32] = 2;   // BookRecordHeader.type: top-of-book
    data.writeUInt32LE(crc32(data.subarray(lastDelta + 12, lastDelta + 12 + size)), lastDelta + 4);
    fs.writeFileSync(file, data);
    rewritten += 1;
  }
  return rewritten;
}

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'lrvx-touch-ctx-'));
const reg = new lrvx.SymbolRegistry();
const sym = reg.addSymbol('sim', 'BTCUSDT', 0.01);

const w = new lrvx.DataWriter(dir, 4, 0);
w.writeBook(SNAP_NS, SNAP_NS, 1n, Number(sym), true,
            levels([[100.0, 1.0], [99.0, 2.0]]), levels([[101.0, 1.0], [102.0, 2.0]]));
// Written as a delta, rewritten below as a top: the touch tightens to
// 100.4 / 100.6 while the depth book keeps the snapshot.
w.writeBook(TOP_NS, TOP_NS, 2n, Number(sym), false,
            levels([[100.4, 3.0]]), levels([[100.6, 2.0]]));
// A trade after the top is the only way a callback sees the context: the
// top itself never dispatches.
w.writeTrade(TRADE_NS, TRADE_NS, 100.5, 0.1, 1, Number(sym), 'buy');
w.close();
assert.strictEqual(rewriteLastDeltaAsTop(dir), 1, 'the fixture rewrote one delta frame');

const atBook = [];
const atTrade = [];
const bt = new lrvx.BacktestRunner(reg, 0, 100000);
bt.setStrategy({
  symbols: [Number(sym)],
  onBookUpdate(ctx) {
    atBook.push([ctx.lastUpdateNs, ctx.lastTouchNs, ctx.bestBid]);
  },
  onTrade(ctx) {
    atTrade.push([ctx.lastUpdateNs, ctx.lastTouchNs, ctx.bestBid, ctx.bestAsk]);
  },
});
assert.ok(bt.runTape(dir), 'the tape ran');

// The depth frame dispatched once and stamped both timestamps.
assert.deepStrictEqual(atBook, [[SNAP_NS, SNAP_NS, 100.0]]);
// The top never dispatched. At the trade, lastUpdateNs is the trade's stamp,
// lastTouchNs is still the top's, and the quotes are the top's.
assert.deepStrictEqual(atTrade, [[TRADE_NS, TOP_NS, 100.4, 100.6]]);
assert.strictEqual(typeof atTrade[0][0], 'bigint', 'nanosecond stamps cross as BigInt');

console.log('test_touch_context OK');
