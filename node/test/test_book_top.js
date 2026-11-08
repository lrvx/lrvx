'use strict';
// A top frame (BookUpdateType.TOP_OF_BOOK) never fires onBookUpdate -- the
// depth book ignores it -- but the touch it carries is what the next
// callback's context quotes. A side is { price, qty } or null; null means
// that side of the book is empty and quotes null.

const assert = require('node:assert');
const path = require('path');
const lrvx = require(path.join(__dirname, '..'));

const reg = new lrvx.SymbolRegistry();
const sym = reg.addSymbol('test', 'BTC', 0.01);

let bookCalls = 0;
const quotes = [];
const runner = new lrvx.Runner(reg, () => {}, false);
runner.addStrategy({
  symbols: [Number(sym)],
  onBookUpdate() {
    bookCalls += 1;
  },
  onTrade(ctx) {
    quotes.push({ bid: ctx.bestBid, ask: ctx.bestAsk });
  },
});
runner.start();

runner.onBookSnapshot(
  Number(sym),
  Float64Array.from([100.00]), Float64Array.from([2.5]),
  Float64Array.from([100.05]), Float64Array.from([1.25]),
  1n,
);
assert.strictEqual(bookCalls, 1);

runner.onBookTop(Number(sym), { price: 100.02, qty: 4.0 }, { price: 100.03, qty: 3.0 }, 2n);
assert.strictEqual(bookCalls, 1, 'a top frame must not fire onBookUpdate');

runner.onTrade(Number(sym), 100.02, 0.1, true, 3n);
assert.strictEqual(quotes.length, 1);
assert.strictEqual(quotes[0].bid, 100.02, 'the touch from the top frame, not the stale snapshot');
assert.strictEqual(quotes[0].ask, 100.03);

assert.throws(
  () => runner.onBookTop(Number(sym), 100.02, null, 4n),
  TypeError,
  'a bare number where a side object belongs must throw, not publish an empty side',
);

runner.onBookTop(Number(sym), { price: 100.02, qty: 4.0 }, null, 4n);
runner.onTrade(Number(sym), 100.02, 0.1, true, 5n);
assert.strictEqual(quotes.length, 2);
assert.strictEqual(quotes[1].bid, 100.02);
assert.strictEqual(quotes[1].ask, null, 'a null side quotes null');

runner.stop();
console.log('test_book_top OK');
