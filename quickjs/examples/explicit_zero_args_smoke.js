// Smoke test: an explicit 0 is not "left out".
//
// The wrappers defaulted an argument with `value || fallback`, so a 0 the
// caller wrote was replaced by the fallback wherever the fallback is not 0:
//   - OrderBook.getBids(0) / getAsks(0) returned 20 levels
//   - DeltaBookEncoder({ anchorEvery: 0 }) ("every event is a snapshot")
//     anchored every 100 events
//   - Partitioner.byTime(0) returned two partitions
// Leaving the argument out still takes the fallback.

let passed = 0;
let failed = 0;
function check(cond, msg) {
    if (cond) { passed++; console.log('  ok  ' + msg); }
    else { failed++; console.log('  FAIL  ' + msg); }
}

// ---- OrderBook levels ----
var book = new OrderBook(0.1);
var bids = [], asks = [], qty = [];
for (var k = 0; k < 30; ++k) {
    bids.push(Math.round((100.0 - k * 0.1) * 10) / 10);
    asks.push(Math.round((100.1 + k * 0.1) * 10) / 10);
    qty.push(1.0);
}
book.applySnapshot(bids, qty, asks, qty);
check(book.getBids(0).length === 0, 'getBids(0) returns no level (got ' + book.getBids(0).length + ')');
check(book.getAsks(0).length === 0, 'getAsks(0) returns no level (got ' + book.getAsks(0).length + ')');
check(book.getBids().length === 20 && book.getAsks().length === 20, 'left out: 20 levels');
check(book.getBids(5).length === 5, 'a count that is given is the count');

// ---- DeltaBookEncoder anchor cadence ----
function lvl(price, q) { return { priceRaw: price, qtyRaw: q }; }
var always = new lrvx.DeltaBookEncoder({ anchorEvery: 0 });
always.encode(1, [lvl(10000, 10)], [lvl(10001, 8)]);
var second = always.encode(1, [lvl(10000, 12)], [lvl(10001, 8)]);
check(second.isDelta === false, 'anchorEvery 0: the second event is a snapshot too');
always.destroy();

var byDefault = new lrvx.DeltaBookEncoder();
byDefault.encode(1, [lvl(10000, 10)], [lvl(10001, 8)]);
check(byDefault.encode(1, [lvl(10000, 12)], [lvl(10001, 8)]).isDelta === true,
      'left out: the second event is a delta');
byDefault.destroy();

// ---- Partitioner.byTime ----
var dir = '/tmp/lrvx-explicit-zero-qjs-smoke-' + Date.now() + '-' + Math.floor(Math.random() * 1000000);
var SEC = 1000000000n;
var w = new DataWriter(dir);
for (var i = 0; i < 10; i++) {
    var ts = BigInt(10 + i) * SEC;
    w.writeTrade(ts, ts, 100.0 + i, 1.0, i + 1, 1, true);
}
w.destroy();
var part = new Partitioner(dir);
check(part.byTime(0).length === 0, 'byTime(0) returns no partition (got ' + part.byTime(0).length + ')');
check(part.byTime().length === 2, 'left out: two partitions (got ' + part.byTime().length + ')');
check(part.byTime(3).length === 3, 'a count that is given is the count');
part.destroy();

if (failed > 0) {
    console.log('\n' + failed + ' check(s) failed');
    throw new Error('explicit zero args smoke test failed');
}
console.log('\n' + passed + ' check(s) passed');
