const fs = require('fs');
const os = require('os');
const path = require('path');
const { TraceRecorder, TraceReader } = require('../');

function scratch(tag) {
  const p = path.join(os.tmpdir(), `lrvx_run_node_${tag}`);
  if (fs.existsSync(p)) {
    fs.rmSync(p, { recursive: true, force: true });
  }
  return p;
}

function check(label, ok) {
  if (!ok) {
    console.error(`FAIL: ${label}`);
    process.exit(1);
  }
  console.log(`ok: ${label}`);
}

function roundTrip() {
  const out = scratch('roundtrip');
  const rec = new TraceRecorder({
    path: out,
    strategyId: 'pair-trade',
    strategyHash: 'sha256:test',
    runStartedNs: 1700000000000000000,
  });
  rec.addTapeRef({ path: 'BTCUSDT.lrvx', contentHash: 'sha256:abc',
                   firstEventNs: 1700000000000000000, lastEventNs: 1700000000500000000 });
  rec.writeSignal({
    runTsNs: 1700000000100000000,
    feedTsNs: 1700000000099000000,
    signalId: 42,
    flags: 1,
    strengthRaw: 75000000,
    name: 'ratio-cross',
    symbolIds: [1, 2],
    payload: Buffer.from('{"src":"ETH","dst":"BTC"}'),
  });
  rec.writeOrderEvent({
    runTsNs: 1700000000200000000,
    feedTsNs: 1700000000099000000,
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
    runTsNs: 1700000000300000000,
    feedTsNs: 1700000000250000000,
    orderId: 7,
    fillId: 12345,
    priceRaw: 5000000000000,
    qtyRaw: 100000000,
    feeRaw: 50000,
    symbolId: 1,
    side: 0,
    liquidity: 1,
  });
  rec.setRunEndedNs(1700000000400000000);
  rec.close();

  const reader = new TraceReader(out);
  check('strategy id', reader.strategyId() === 'pair-trade');
  check('run ended', reader.runEndedNs() === 1700000000400000000n);
  const refs = reader.tapeRefs();
  check('tape ref count', refs.length === 1);
  check('tape ref path', refs[0].path === 'BTCUSDT.lrvx');

  const sigs = reader.readAllSignals();
  check('signal count', sigs.length === 1);
  check('signal id', sigs[0].signalId === 42);
  check('signal name', sigs[0].name === 'ratio-cross');
  check('signal symbol ids', sigs[0].symbolIds.length === 2 &&
                              sigs[0].symbolIds[0] === 1 && sigs[0].symbolIds[1] === 2);
  check('signal payload', Buffer.from(sigs[0].payload).toString() === '{"src":"ETH","dst":"BTC"}');

  const orders = reader.readAllOrderEvents();
  check('order count', orders.length === 1);
  check('order id', orders[0].orderId === 7n);
  check('order parent', orders[0].parentSignalId === 42n);
  check('order kind', orders[0].eventKind === 1);
  check('order flags', orders[0].flags === 1);

  const fills = reader.readAllFills();
  check('fill count', fills.length === 1);
  check('fill id', fills[0].fillId === 12345n);
  check('fill liquidity', fills[0].liquidity === 1);

  fs.rmSync(out, { recursive: true, force: true });
}

function multiSymbolPreserved() {
  const out = scratch('multisym');
  const rec = new TraceRecorder({ path: out, strategyId: 'pair' });
  rec.writeSignal({ runTsNs: 1000, name: 'pair-trade', symbolIds: [10, 20, 30, 40], payload: Buffer.alloc(0) });
  rec.close();
  const reader = new TraceReader(out);
  const sigs = reader.readAllSignals();
  check('multi-symbol count', sigs[0].symbolIds.length === 4);
  check('multi-symbol values', sigs[0].symbolIds.every((v, i) => v === [10, 20, 30, 40][i]));
  fs.rmSync(out, { recursive: true, force: true });
}

function emptyRunRoundTrips() {
  const out = scratch('empty');
  const rec = new TraceRecorder({ path: out, strategyId: 'empty' });
  rec.close();
  const reader = new TraceReader(out);
  check('empty signals', reader.readAllSignals().length === 0);
  check('empty orders', reader.readAllOrderEvents().length === 0);
  check('empty fills', reader.readAllFills().length === 0);
  fs.rmSync(out, { recursive: true, force: true });
}

roundTrip();
multiSymbolPreserved();
emptyRunRoundTrips();
console.log('all node run-trace checks passed');
