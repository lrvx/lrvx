'use strict';
/**
 * node/test/test_registry_trace_segment_reports.js -- three things a caller
 * could not read back:
 *
 *   - the tick and lot a registry entry holds (SymbolRegistry.tickSize /
 *     tickSizeDeclared / lotSize);
 *   - the whole id of a captured signal (TraceSignalRecord.signalId64): the
 *     record kept the low 32 bits of the order id;
 *   - where mergeDir wrote and what it skipped (outputPath, errors), and
 *     validate() with a check turned off: the flags were ignored.
 *
 * Run from repo root:
 *   cd node && node test/test_registry_trace_segment_reports.js
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

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'lrvx-reports-'));

console.log('=== the registry reads back an entry\'s tick and lot ===');
{
  const reg = new lrvx.SymbolRegistry();
  const plain = reg.addSymbol('test', 'PLAIN');
  const ticked = reg.addSymbol('test', 'TICKED', 0.5);
  const both = reg.addSymbol('test', 'BOTH', 0.01, 0.001);

  check(reg.tickSize(plain) === 0.01 && reg.tickSizeDeclared(plain) === false,
        'no tick given: the book\'s 0.01, not declared');
  check(reg.lotSize(plain) === null, 'no lot given: null');
  check(reg.tickSize(ticked) === 0.5 && reg.tickSizeDeclared(ticked) === true,
        'a tick given is declared');
  check(reg.lotSize(ticked) === null, 'and its lot is still null');
  check(reg.tickSize(both) === 0.01 && reg.tickSizeDeclared(both) === true,
        'an explicit 0.01 is declared');
  check(reg.lotSize(both) === 0.001, 'the lot reads back');
  check(reg.lotSize(Number(both)) === 0.001 && reg.tickSize(Number(ticked)) === 0.5,
        'a numeric id asks the same question');

  for (const method of ['tickSize', 'tickSizeDeclared', 'lotSize']) {
    let threw = null;
    try { reg[method](9999); } catch (e) { threw = e; }
    check(threw instanceof RangeError && String(threw).includes('9999'),
          `${method}(9999) is a RangeError naming the id`);
    threw = null;
    try { reg[method](); } catch (e) { threw = e; }
    check(threw instanceof TypeError, `${method}() is a TypeError`);
  }
}

console.log('=== a captured signal keeps its whole order id ===');
{
  const dir = path.join(root, 'run.lrvxrun');
  const reg = new lrvx.SymbolRegistry();
  const sym = reg.addSymbol('test', 'BTCUSDT', 0.01);
  const rec = new lrvx.TraceRecorder({path: dir, strategyId: 'capture'});
  const runner = new lrvx.Runner(reg, () => {}, false);
  const ids = [];
  runner.addStrategy({
    symbols: [sym],
    onTrade(_ctx, _trade, emit) { if (ids.length === 0) { ids.push(emit.limitSell(101, 1)); } },
  });
  runner.attachTraceRecorder(rec);
  runner.start();
  runner.onTrade(Number(sym), 100, 1, true, 1000n);
  runner.stop();
  runner.attachTraceRecorder(null);
  rec.writeSignal({runTsNs: 5n, signalId: 42, name: 'manual', symbolIds: [1]});
  rec.close();

  const reader = new lrvx.TraceReader(dir);
  const sigs = reader.readAllSignals();
  reader.close();
  check(sigs.length === 2, `two signals (${sigs.length})`);
  check(ids[0] >= 4294967296, `the order id carries the subscriber id (${ids[0]})`);
  check(sigs[0].signalId64 === BigInt(ids[0]), `signalId64 is the order id (${sigs[0].signalId64})`);
  check(sigs[0].signalId === ids[0] % 4294967296,
        `signalId is still its low 32 bits (${sigs[0].signalId})`);
  check(sigs[1].signalId === 42 && sigs[1].signalId64 === 42n,
        'a small id reads the same through both');
}

console.log('=== mergeDir says where it wrote and what it skipped ===');
{
  const dir = path.join(root, 'tape');
  const w = new lrvx.DataWriter(dir, 16, 1);
  for (let i = 0; i < 5; i++) {
    const ts = BigInt(10 + i) * 1000000000n;
    w.writeTrade(ts, ts, 100 + i, 1, i + 1, 1, 'buy');
  }
  w.close();

  const out = path.join(root, 'merged');
  const clean = lrvx.mergeDir(dir, out);
  check(clean.success === true && clean.segmentsMerged === 1 && clean.eventsWritten === 5,
        'the counts are what they were');
  check(typeof clean.outputPath === 'string' && clean.outputPath.startsWith(out) &&
        fs.existsSync(clean.outputPath), `outputPath is the segment written: ${clean.outputPath}`);
  check(Array.isArray(clean.errors) && clean.errors.length === 0, 'no errors: an empty array');

  const bad = path.join(dir, 'zz_bad.lrvx');
  fs.writeFileSync(bad, 'x'.repeat(256));
  const partial = lrvx.mergeDir(dir, path.join(root, 'merged2'));
  check(partial.segmentsMerged === 1, 'the segment that opens is still merged');
  check(partial.errors.length === 1 && partial.errors[0] === `Cannot open: ${bad}`,
        `the one that does not is listed: ${JSON.stringify(partial.errors)}`);
  check(partial.success === false, 'and success is false, as the errors say');
  fs.rmSync(bad);

  const none = lrvx.mergeDir(path.join(root, 'no_such_dir'), path.join(root, 'merged3'));
  check(none.success === false && none.errors.length >= 1 && none.outputPath === '',
        `nothing to merge: ${JSON.stringify(none.errors)}`);
}

console.log('=== validate() honours a check that is turned off ===');
{
  // Timestamps that go backwards: an anomaly the timestamp check counts.
  const dir = path.join(root, 'backwards');
  const w = new lrvx.DataWriter(dir, 16, 1);
  const stamps = [30n, 10n, 20n].map((s) => s * 1000000000n);
  stamps.forEach((ts, i) => w.writeTrade(ts, ts, 100 + i, 1, i + 1, 1, 'buy'));
  w.close();
  const segment = path.join(dir, fs.readdirSync(dir).find((f) => f.endsWith('.lrvx')));

  const both = lrvx.validate(segment);
  check(both.timestampAnomalies > 0, `omitted: timestamps are checked (${both.timestampAnomalies})`);
  check(lrvx.validate(segment, true, true).timestampAnomalies === both.timestampAnomalies,
        'true, true is the same');
  check(lrvx.validate(segment, true, false).timestampAnomalies === 0,
        'verifyTimestamps false turns the check off');
  check(lrvx.validate(segment, undefined, undefined).timestampAnomalies === both.timestampAnomalies,
        'undefined counts as omitted');

  // One flipped byte in the first trade's price (a 64-byte segment header,
  // a 12-byte frame header, then the record): a CRC error the CRC check
  // counts.
  const bytes = fs.readFileSync(segment);
  bytes[64 + 12 + 20] ^= 0xff;
  const corrupt = path.join(root, 'corrupt.lrvx');
  fs.writeFileSync(corrupt, bytes);
  const checked = lrvx.validate(corrupt);
  const unchecked = lrvx.validate(corrupt, false);
  check(checked.crcErrors > 0, `omitted: CRCs are checked (${checked.crcErrors})`);
  check(unchecked.crcErrors === 0, `verifyCrc false turns the check off (${unchecked.crcErrors})`);
}

fs.rmSync(root, {recursive: true, force: true});

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed > 0 ? 1 : 0);
