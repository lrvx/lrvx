// Round-trip smoke test for the .lrvxrun bindings under QuickJS.

var path = '/tmp/lrvx_run_qjs_smoke_' + Date.now();

function check(label, ok) {
  if (!ok) {
    console.log('FAIL: ' + label);
    throw new Error('smoke fail: ' + label);
  }
  console.log('ok: ' + label);
}

var rec = new lrvx.TraceRecorder({
  path: path,
  strategyId: 'qjs-smoke',
  strategyHash: 'sha256:test',
  runStartedNs: 1700000000000000000n,
});
rec.addTapeRef({
  path: 'BTCUSDT.lrvx',
  contentHash: 'sha256:abc',
  firstEventNs: 1700000000000000000n,
  lastEventNs: 1700000000500000000n,
});
rec.writeSignal({
  runTsNs: 1700000000100000000n,
  feedTsNs: 1700000000099000000n,
  signalId: 42,
  flags: 1,
  strengthRaw: 75000000,
  name: 'ratio-cross',
  symbolIds: [1, 2],
  payload: '{"src":"ETH","dst":"BTC"}',
});
rec.writeOrderEvent({
  runTsNs: 1700000000200000000n,
  feedTsNs: 1700000000099000000n,
  orderId: 7,
  parentSignalId: 42,
  priceRaw: 5000000000000,
  qtyRaw: 100000000,
  symbolId: 1,
  eventKind: 1,
  side: 0,
  orderType: 1,
  flags: 1,
  reason: '',
});
rec.writeFill({
  runTsNs: 1700000000300000000n,
  feedTsNs: 1700000000250000000n,
  orderId: 7,
  fillId: 12345,
  priceRaw: 5000000000000,
  qtyRaw: 100000000,
  feeRaw: 50000,
  symbolId: 1,
  side: 0,
  liquidity: 1,
});
rec.setRunEndedNs(1700000000400000000n);
rec.close();

var reader = new lrvx.TraceReader(path);
check('strategy id', reader.strategyId() === 'qjs-smoke');
check('run ended', reader.runEndedNs() === 1700000000400000000n);

var sigs = reader.readAllSignals();
check('signal count', sigs.length === 1);
check('signal id', sigs[0].signalId === 42);
check('signal name', sigs[0].name === 'ratio-cross');
check('signal symbol ids', sigs[0].symbolIds.length === 2 &&
                          sigs[0].symbolIds[0] === 1 && sigs[0].symbolIds[1] === 2);
check('signal payload', sigs[0].payload === '{"src":"ETH","dst":"BTC"}');

var orders = reader.readAllOrderEvents();
check('order count', orders.length === 1);
check('order id', orders[0].orderId === 7n);
check('order parent', orders[0].parentSignalId === 42n);
check('order kind', orders[0].eventKind === 1);

var fills = reader.readAllFills();
check('fill count', fills.length === 1);
check('fill id', fills[0].fillId === 12345n);
check('fill liquidity', fills[0].liquidity === 1);

reader.destroy();

// An id above 32 bits is written whole, as a Number or as a BigInt. It used
// to be cut to its low half.
var widePath = path + '_wide';
var wide = new lrvx.TraceRecorder({ path: widePath, strategyId: 'qjs-wide' });
wide.writeSignal({ runTsNs: 1n, signalId: 7 * 4294967296 + 5, name: 'number' });
wide.writeSignal({ runTsNs: 2n, signalId: (7n << 32n) + 6n, name: 'bigint' });
wide.writeSignal({ runTsNs: 3n, signalId: 4294967295, name: 'small' });
wide.close();
var wideReader = new lrvx.TraceReader(widePath);
var wideSigs = wideReader.readAllSignals();
check('a wide Number id reads back whole', wideSigs[0].signalId64 === (7n << 32n) + 5n);
check('and its low half is signalId', wideSigs[0].signalId === 5);
check('a wide BigInt id reads back whole', wideSigs[1].signalId64 === (7n << 32n) + 6n);
check('an id below 2^32 is unchanged',
      wideSigs[2].signalId === 4294967295 && wideSigs[2].signalId64 === 4294967295n);
wideReader.destroy();

// The ids of an order event and a fill read back as BigInt, whole: an order
// id is (subscriber id << 32) + n, and with a subscriber id of 2^21 or more
// it is past what a Number holds exactly.
var bigPath = path + '_big';
var BIG = ((2n ** 21n + 3n) << 32n) + 7n;
var big = new lrvx.TraceRecorder({ path: bigPath, strategyId: 'qjs-big' });
big.writeOrderEvent({ runTsNs: 1n, orderId: BIG, parentSignalId: BIG - 1n, symbolId: 1 });
big.writeFill({ runTsNs: 2n, orderId: BIG, fillId: BIG + 5n, symbolId: 1 });
big.close();
var bigReader = new lrvx.TraceReader(bigPath);
var bigOrders = bigReader.readAllOrderEvents();
var bigFills = bigReader.readAllFills();
check('an order id above 2^53 reads back whole', bigOrders[0].orderId === BIG);
check('and so does its parent signal id', bigOrders[0].parentSignalId === BIG - 1n);
check('a fill keeps its order id and fill id whole',
      bigFills[0].orderId === BIG && bigFills[0].fillId === BIG + 5n);
bigReader.destroy();

console.log('quickjs run-trace smoke ok');
