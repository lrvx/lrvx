'use strict';
/**
 * node/test/test_trace_ids_and_argument_checks.js
 *
 *   - TraceRecorder.writeOrderEvent / writeFill and Runner.traceOrderEvent /
 *     traceFill take orderId, parentSignalId and fillId as a Number or a
 *     BigInt and keep all 64 bits; a value that is not an id is a TypeError.
 *     They were read as a Number through Int64Value(): a BigInt threw
 *     "A number was expected", and NaN, a fraction or a negative was written
 *     as whatever the conversion made of it.
 *   - TraceReader returns signalId64, orderId, parentSignalId and fillId as
 *     BigInt. As a Number an id from a strategy with a subscriber id of 2^21
 *     or more (order ids are (subscriber id << 32) + n) lost its low bits.
 *   - The non-emit reads of a symbol, a bar type and the like throw a
 *     TypeError that names the argument.
 *
 * Run from repo root:
 *   cd node && node test/test_trace_ids_and_argument_checks.js
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
function typeErrorNaming(fn, word, msg) {
  try {
    fn();
    check(false, `${msg}: nothing was thrown`);
  } catch (e) {
    check(e instanceof TypeError && String(e.message).includes(word),
          `${msg} (${e.constructor.name}: ${e.message})`);
  }
}

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'lrvx-trace-ids-'));
// An order id of a strategy whose subscriber id is 2^21 + 3: above 2^53.
const WIDE = ((2n ** 21n + 3n) << 32n) + 7n;

console.log('=== 64-bit ids survive a trace, written as BigInt ===');
{
  const dir = path.join(root, 'wide.lrvxrun');
  const rec = new lrvx.TraceRecorder({path: dir, strategyId: 'wide'});
  rec.writeSignal({runTsNs: 1n, signalId: WIDE, name: 'sig'});
  rec.writeOrderEvent({runTsNs: 2n, orderId: WIDE, parentSignalId: WIDE - 1n, symbolId: 1,
                       priceRaw: 5000000000000n, qtyRaw: 100000000});
  rec.writeOrderEvent({runTsNs: 3n, orderId: 7, parentSignalId: 42, symbolId: 1});
  rec.writeFill({runTsNs: 4n, orderId: WIDE, fillId: WIDE + 5n, symbolId: 1,
                 priceRaw: 5000000000000n, qtyRaw: 100000000n, feeRaw: 12n});
  rec.close();

  const reader = new lrvx.TraceReader(dir);
  const sigs = reader.readAllSignals();
  const orders = reader.readAllOrderEvents();
  const fills = reader.readAllFills();
  reader.close();

  check(sigs[0].signalId64 === WIDE, `signalId64 is the whole id, a BigInt (${sigs[0].signalId64})`);
  check(orders[0].orderId === WIDE, `orderId is the whole id (${orders[0].orderId})`);
  check(orders[0].parentSignalId === WIDE - 1n,
        `parentSignalId is the whole id (${orders[0].parentSignalId})`);
  check(orders[0].priceRaw === 5000000000000, 'priceRaw written as a BigInt reads back');
  check(orders[1].orderId === 7n && orders[1].parentSignalId === 42n,
        'ids written as Numbers read back as BigInt too');
  check(fills[0].orderId === WIDE && fills[0].fillId === WIDE + 5n,
        `a fill's orderId and fillId are whole (${fills[0].orderId}, ${fills[0].fillId})`);
  check(fills[0].feeRaw === 12, 'feeRaw written as a BigInt reads back');
}

console.log('=== a value that is not an id is refused ===');
{
  const dir = path.join(root, 'bad.lrvxrun');
  const rec = new lrvx.TraceRecorder({path: dir, strategyId: 'bad'});
  for (const [what, bad] of [['NaN', NaN], ['a fraction', 1.5], ['a negative', -1],
                             ['a string', '7'], ['2^53 + 2 as a Number', 2 ** 53 + 2],
                             ['a negative BigInt', -1n], ['2^64 as a BigInt', 2n ** 64n]]) {
    typeErrorNaming(() => rec.writeOrderEvent({runTsNs: 1n, orderId: bad, symbolId: 1}),
                    'orderId', `writeOrderEvent refuses orderId ${what}`);
  }
  typeErrorNaming(() => rec.writeOrderEvent({runTsNs: 1n, orderId: 1, parentSignalId: 0.5}),
                  'parentSignalId', 'writeOrderEvent refuses a fractional parentSignalId');
  typeErrorNaming(() => rec.writeOrderEvent({runTsNs: 1n, orderId: 1, priceRaw: 1.5}),
                  'priceRaw', 'writeOrderEvent refuses a fractional priceRaw');
  typeErrorNaming(() => rec.writeFill({runTsNs: 1n, orderId: 1, fillId: NaN}),
                  'fillId', 'writeFill refuses fillId NaN');
  typeErrorNaming(() => rec.writeFill({runTsNs: 1n, orderId: 1, symbolId: -3}),
                  'symbolId', 'writeFill refuses a negative symbolId');
  typeErrorNaming(() => rec.writeFill(), 'options', 'writeFill refuses a missing argument');
  typeErrorNaming(() => rec.writeSignal({runTsNs: 1n, signalId: 1.5}),
                  'signalId', 'writeSignal refuses a fractional signalId');
  rec.close();
  const reader = new lrvx.TraceReader(dir);
  check(reader.readAllOrderEvents().length === 0 && reader.readAllFills().length === 0 &&
            reader.readAllSignals().length === 0,
        'nothing a refused call named was written');
  reader.close();
}

console.log('=== Runner.traceOrderEvent / traceFill ===');
{
  const dir = path.join(root, 'runner.lrvxrun');
  const reg = new lrvx.SymbolRegistry();
  const sym = reg.addSymbol('test', 'BTCUSDT', 0.01);
  const runner = new lrvx.Runner(reg, () => {});
  const rec = new lrvx.TraceRecorder({path: dir, strategyId: 'runner'});
  runner.attachTraceRecorder(rec);
  runner.traceOrderEvent({orderId: WIDE, parentSignalId: WIDE - 1n, symbolId: 1, eventKind: 1,
                          side: 0, price: 100.5, qty: 1});
  runner.traceFill({orderId: WIDE, fillId: WIDE + 9n, price: 100.5, qty: 1, symbolId: 1, side: 0});
  typeErrorNaming(() => runner.traceOrderEvent({orderId: NaN, symbolId: 1, eventKind: 1, side: 0}),
                  'orderId', 'traceOrderEvent refuses orderId NaN');
  typeErrorNaming(() => runner.traceFill({orderId: 1, fillId: -2, price: 1, qty: 1, symbolId: 1,
                                          side: 0}),
                  'fillId', 'traceFill refuses a negative fillId');
  typeErrorNaming(() => runner.traceFill({orderId: 1, price: '1', qty: 1, symbolId: 1, side: 0}),
                  'price', 'traceFill refuses a price that is not a number');
  rec.close();

  const reader = new lrvx.TraceReader(dir);
  const orders = reader.readAllOrderEvents();
  const fills = reader.readAllFills();
  reader.close();
  check(orders.length === 1 && orders[0].orderId === WIDE &&
            orders[0].parentSignalId === WIDE - 1n,
        'traceOrderEvent writes the whole ids');
  check(fills.length === 1 && fills[0].orderId === WIDE && fills[0].fillId === WIDE + 9n,
        'traceFill writes the whole ids');

  console.log('=== non-emit arguments ===');
  typeErrorNaming(() => runner.onTrade('BTC', 100, 1, true, 1n), 'symbol',
                  'onTrade refuses a symbol that is a string');
  typeErrorNaming(() => runner.onTrade(sym, '100', 1, true, 1n), 'price',
                  'onTrade refuses a price that is a string');
  typeErrorNaming(() => runner.onTrade(sym, 100, 1, 'yes', 1n), 'isBuy',
                  'onTrade refuses an isBuy that is not a boolean');
  typeErrorNaming(() => runner.onTrade(1.5, 100, 1, true, 1n), 'symbol',
                  'onTrade refuses a fractional symbol id');
  typeErrorNaming(() => runner.onTrade({id: 'x'}, 100, 1, true, 1n), 'symbol',
                  'onTrade refuses an object whose id is not a number');

  let bars = null;
  runner.addStrategy({
    symbols: [sym],
    onTrade(ctx, trade, emit) {
      bars = [];
      for (const call of [() => emit.lastClosedBar(sym, 'time', 60),
                          () => emit.lastClosedBar(sym, 0, -1),
                          () => emit.lastClosedBar('BTC', 0, 60),
                          () => emit.lastNClosedBars(sym, 0, 60, 1.5),
                          () => emit.setBarRingCapacity('many')]) {
        try { call(); bars.push('no throw'); }
        catch (e) { bars.push(e instanceof TypeError ? e.message : `not a TypeError: ${e.message}`); }
      }
      bars.push(emit.lastClosedBar(sym, 0, 60));
    },
  });
  runner.start();
  runner.onTrade(sym, 100, 1, true, 1n);
  runner.stop();
  check(bars !== null && bars.length === 6, 'the strategy ran its checks');
  if (bars !== null && bars.length === 6) {
    check(bars[0].includes('barType'), `lastClosedBar refuses a string bar type (${bars[0]})`);
    check(bars[1].includes('barTypeParam'), `lastClosedBar refuses a negative param (${bars[1]})`);
    check(bars[2].includes('symbol'), `lastClosedBar refuses a string symbol (${bars[2]})`);
    check(bars[3].includes('lastNClosedBars: n'), `lastNClosedBars refuses a fractional n (${bars[3]})`);
    check(bars[4].includes('capacity'), `setBarRingCapacity refuses a string (${bars[4]})`);
    check(bars[5] === null, 'a well-formed lastClosedBar still answers');
  }
}

fs.rmSync(root, {recursive: true, force: true});
console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed === 0 ? 0 : 1);
