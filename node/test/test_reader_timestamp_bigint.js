'use strict';
/**
 * node/test/test_reader_timestamp_bigint.js -- every record a tape reader
 * hands back carries its clock readings as BigInt, to the nanosecond.
 *
 * A wall-clock nanosecond reading is near 1.76e18, past 2^53, where a JS
 * Number steps 256 ns at a time. Records written 1 ns and 100 ns apart must
 * come back distinct, in order, and equal to what was written. Covers
 * DataReader.readTrades / readTradesFrom / readBBO / readBBOFrom /
 * readBookUpdates / readBookUpdatesFrom and MergedTapeReader.readTrades /
 * readBooks, which share the record shapes.
 *
 * Run from repo root:
 *   cd node && node test/test_reader_timestamp_bigint.js
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

// Three readings inside one 256 ns double step.
const T0 = 1757000000123456789n;
const EXCHANGE_TS = [T0, T0 + 1n, T0 + 100n];
const RECV_LAG = 7n;

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'lrvx-reader-ts-'));
const tradeDir = path.join(dir, 'trades');
const bookDir = path.join(dir, 'books');
fs.mkdirSync(tradeDir);
fs.mkdirSync(bookDir);

const tw = new lrvx.DataWriter(tradeDir, 4, 1);
EXCHANGE_TS.forEach((ts, i) => {
  tw.writeTrade(ts, ts + RECV_LAG, 100 + i, 1, i + 1, 1, 'buy');
});
tw.close();

const bw = new lrvx.DataWriter(bookDir, 4, 1);
const level = (price) => BigInt64Array.from([BigInt(price) * 100000000n, 100000000n]);
EXCHANGE_TS.forEach((ts, i) => {
  bw.writeBook(ts, ts + RECV_LAG, BigInt(i + 1), 1, i === 0, level(99), level(101));
});
bw.close();

function checkRecords(label, records, expectedTs) {
  check(records.length === expectedTs.length,
        `${label}: ${expectedTs.length} records (got ${records.length})`);
  const n = Math.min(records.length, expectedTs.length);
  let typed = n > 0;
  let exact = n > 0;
  for (let i = 0; i < n; i++) {
    if (typeof records[i].exchangeTsNs !== 'bigint' || typeof records[i].recvTsNs !== 'bigint') {
      typed = false;
    }
    if (String(records[i].exchangeTsNs) !== String(expectedTs[i]) ||
        String(records[i].recvTsNs) !== String(expectedTs[i] + RECV_LAG)) {
      exact = false;
    }
  }
  check(typed, `${label}: exchangeTsNs and recvTsNs are bigint`);
  check(exact, `${label}: both timestamps come back to the nanosecond ` +
               `(${records.slice(0, n).map((r) => String(r.exchangeTsNs)).join(', ')})`);
}

console.log('=== DataReader ===');
{
  const r = new lrvx.DataReader(tradeDir);
  checkRecords('readTrades', r.readTrades(), EXCHANGE_TS);
  checkRecords('readTradesFrom', r.readTradesFrom(T0 + 1n), EXCHANGE_TS.slice(1));
}
{
  const r = new lrvx.DataReader(bookDir);
  checkRecords('readBBO', r.readBBO(), EXCHANGE_TS);
  checkRecords('readBBOFrom', r.readBBOFrom(T0 + 1n), EXCHANGE_TS.slice(1));
  checkRecords('readBookUpdates', r.readBookUpdates(), EXCHANGE_TS);
  checkRecords('readBookUpdatesFrom', r.readBookUpdatesFrom(T0 + 1n), EXCHANGE_TS.slice(1));
}

console.log('=== MergedTapeReader ===');
{
  // Recorded through a Runner: a merged reader keys symbols by the tape's
  // manifest, which DataWriter does not write.
  const merged = path.join(dir, 'merged');
  const reg = new lrvx.SymbolRegistry();
  const sym = reg.addSymbol('test', 'AAA', 0.01);
  const hook = new lrvx.BinaryLogRecorderHook(merged, 4, 1, 'none', 'test', 'spot');
  hook.addSymbol(Number(sym), 'AAA', '', '', 2, 6);
  const runner = new lrvx.Runner(reg, () => {}, false);
  runner.addStrategy({symbols: [sym]});
  runner.setMarketDataRecorder(hook);
  runner.start();
  const bookTs = EXCHANGE_TS.map((t) => t + 1000n);
  for (const ts of EXCHANGE_TS) {
    runner.onTrade(Number(sym), 100, 1, true, ts);
  }
  for (const ts of bookTs) {
    runner.onBookSnapshot(Number(sym), Float64Array.from([99]), Float64Array.from([1]),
                          Float64Array.from([101]), Float64Array.from([1]), ts);
  }
  runner.stop();
  hook.flush();

  const m = new lrvx.MergedTapeReader([merged]);
  const single = new lrvx.DataReader(merged);
  for (const [label, got, want, expected] of [
    ['readTrades', m.readTrades(), single.readTrades(), EXCHANGE_TS],
    ['readBooks', m.readBooks(), single.readBookUpdates(), bookTs],
  ]) {
    check(got.length === expected.length,
          `${label}: ${expected.length} records (got ${got.length})`);
    const n = Math.min(got.length, want.length, expected.length);
    let typed = n > 0;
    let exact = n > 0;
    let same = n > 0;
    for (let i = 0; i < n; i++) {
      if (typeof got[i].exchangeTsNs !== 'bigint' || typeof got[i].recvTsNs !== 'bigint') {
        typed = false;
      }
      if (String(got[i].exchangeTsNs) !== String(expected[i])) { exact = false; }
      if (got[i].exchangeTsNs !== want[i].exchangeTsNs || got[i].recvTsNs !== want[i].recvTsNs) {
        same = false;
      }
    }
    check(typed, `${label}: exchangeTsNs and recvTsNs are bigint`);
    check(exact, `${label}: exchangeTsNs comes back to the nanosecond`);
    check(same, `${label}: both timestamps equal DataReader's for the same record`);
  }
}

console.log('=== a record timestamp goes straight back into a reader ===');
{
  const r = new lrvx.DataReader(tradeDir);
  const all = r.readTrades();
  const last = all.length > 0 ? all[all.length - 1].exchangeTsNs : 0n;
  const from = new lrvx.DataReader(tradeDir).readTradesFrom(last);
  check(from.length === 1 && String(from[0].exchangeTsNs) === String(T0 + 100n),
        'readTradesFrom(record.exchangeTsNs) starts at that record');
}

fs.rmSync(dir, {recursive: true, force: true});

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed > 0 ? 1 : 0);
