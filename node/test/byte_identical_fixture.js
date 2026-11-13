// Write the same .lrvxrun records through the NAPI binding as
// scripts/lrvxrun_byte_identical_fixture.py. The cross-binding gate reads
// them back through the pybind11 TraceReader and compares the decoded
// records field by field, timestamps included: they are BigInt here, and
// each is an odd number of nanoseconds, which a JS Number could not hold.

const lrvx = require('..');

function writeFixture(outDir) {
  const rec = new lrvx.TraceRecorder({
    path: outDir,
    strategyId: 'byte-identical-gate',
    strategyHash: 'sha256:fixture',
    runStartedNs: 1700000000000000001n,
  });
  rec.writeSignal({
    runTsNs: 1700000000100000001n,
    feedTsNs: 1700000000099000001n,
    signalId: 42,
    flags: 1, // SIGNAL_FLAG_ENTER
    strengthRaw: 75000000,
    name: 'entry',
    symbolIds: [1, 2],
    payload: Buffer.alloc(0),
  });
  rec.writeSignal({
    runTsNs: 1700000000200000001n,
    feedTsNs: 1700000000199000001n,
    signalId: 43,
    flags: 2, // SIGNAL_FLAG_EXIT
    strengthRaw: 0,
    name: 'exit',
    symbolIds: [1],
    payload: Buffer.alloc(0),
  });
  rec.writeOrderEvent({
    runTsNs: 1700000000150000001n,
    feedTsNs: 1700000000149000001n,
    orderId: 7,
    parentSignalId: 42,
    priceRaw: 5000000000000,
    qtyRaw: 100000000,
    symbolId: 1,
    eventKind: 1, // Submit (matches OrderEventKind in run_format_v1.h)
    side: 0,
    orderType: 1,
    flags: 0,
    reason: '',
  });
  rec.writeFill({
    runTsNs: 1700000000175000001n,
    feedTsNs: 1700000000174000001n,
    orderId: 7,
    fillId: 12345,
    priceRaw: 5000000000000,
    qtyRaw: 100000000,
    feeRaw: 50000,
    symbolId: 1,
    side: 0,
    liquidity: 1, // Maker
  });
  rec.setRunEndedNs(1700000000400000001n);
  rec.close();
}

const outDir = process.argv[2];
if (!outDir) {
  console.error(`usage: node ${process.argv[1]} <out-dir>`);
  process.exit(2);
}
writeFixture(outDir);
