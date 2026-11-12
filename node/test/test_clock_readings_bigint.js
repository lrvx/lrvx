'use strict';
/**
 * node/test/test_clock_readings_bigint.js -- nanosecond clock readings the
 * addon hands back outside the tape record shapes arrive as BigInt, to the
 * nanosecond.
 *
 * A wall-clock nanosecond reading is near 1.76e18, past 2^53, where a JS
 * Number steps 256 ns at a time; every value checked here is chosen so a
 * double cannot hold it. Covers DataReader.summary / inspect, Partitioner,
 * the DataReader.run onProgress cursor, validateDataset, the order events a
 * strategy receives, OrderJourneyTracer rows, execution-algo child orders,
 * RateLimitPolicy.banUntilNs, FeeSchedule.tierTransitions, TraceReader and
 * tapeDiff. Durations (latencies, intervals, staleness) stay numbers.
 *
 * Run from repo root:
 *   cd node && node test/test_clock_readings_bigint.js
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
function exact(value, want, msg) {
  check(typeof value === 'bigint' && value === want,
        `${msg} (got ${typeof value} ${String(value)}, want ${want})`);
}

// Odd readings: none is a multiple of the 256 ns double step.
const T0 = 1757000000123456789n;
const SEC = 1000000000n;
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'lrvx-clock-readings-'));

console.log('=== DataReader.summary / inspect / validateDataset / Partitioner ===');
{
  const dir = path.join(root, 'tape');
  fs.mkdirSync(dir);
  const w = new lrvx.DataWriter(dir, 4, 1);
  const N = 20000;
  for (let i = 0; i < N; i++) {
    const ts = T0 + BigInt(i) * 1001n;
    w.writeTrade(ts, ts, 100 + (i % 7), 1, i + 1, 1, 'buy');
  }
  w.close();
  const first = T0;
  const last = T0 + BigInt(N - 1) * 1001n;

  const summary = new lrvx.DataReader(dir).summary();
  exact(summary.firstEventNs, first, 'summary().firstEventNs');
  exact(summary.lastEventNs, last, 'summary().lastEventNs');
  check(typeof summary.totalEvents === 'number' && summary.totalEvents === N,
        'summary().totalEvents stays a number');

  const seen = lrvx.inspect(dir);
  exact(seen.firstEventNs, first, 'inspect().firstEventNs');
  exact(seen.lastEventNs, last, 'inspect().lastEventNs');

  const valid = lrvx.validateDataset(dir);
  exact(valid.firstTimestamp, first, 'validateDataset().firstTimestamp');
  exact(valid.lastTimestamp, last, 'validateDataset().lastTimestamp');

  const parts = new lrvx.Partitioner(dir).byTime(2, 0);
  check(parts.length === 2, `byTime(2) returns two partitions (${parts.length})`);
  const p = parts[0] || {};
  exact(p.fromNs, first, 'partition.fromNs');
  exact(p.warmupFromNs, first, 'partition.warmupFromNs');
  check(typeof p.toNs === 'bigint' && p.toNs > first && p.toNs <= last + 1n,
        `partition.toNs is a bigint inside the tape (${typeof p.toNs} ${String(p.toNs)})`);
  // A partition's bounds go straight back into a reader.
  const sub = new lrvx.DataReader(dir, p.fromNs, p.toNs);
  check(sub.readTrades().length > 0, 'a reader opens on a partition\'s bounds');

  const cursors = [];
  const agg = new lrvx.OHLCBinAggregator(60000000000n, false);
  new lrvx.DataReader(dir).run([agg], {
    nThreads: 1,
    progressIntervalMs: 0,
    onProgress: (_pct, ts) => { cursors.push(ts); return true; },
  });
  check(cursors.length > 0, `onProgress fired (${cursors.length} calls)`);
  check(cursors.length > 0 && cursors.every((c) => typeof c === 'bigint'),
        'the onProgress cursor is a bigint');
  check(cursors.length > 0 && cursors.every((c) => c == 0 || (c >= first && c <= last)),
        'the onProgress cursor is a reading from the tape');
  check(cursors.some((c) => typeof c === 'bigint' && c % 256n !== 0n),
        'a cursor value a double could not hold arrives intact');
}

console.log('=== order events and the journey tracer ===');
{
  const reg = new lrvx.SymbolRegistry();
  const sym = reg.addSymbol('test', 'AAA', 0.5);
  const bt = new lrvx.BacktestRunner(reg, 0, 10000);
  const tracer = new lrvx.OrderJourneyTracer();
  bt.addJourneyTracer(tracer);
  const events = [];
  let seen = 0;
  bt.setStrategy({
    symbols: [sym],
    onBar(_ctx, _bar, emit) {
      seen += 1;
      if (seen === 1) { emit.limitBuy(99, 1); }
      if (seen === 4) { emit.limitSell(150, 1); }
    },
    onOrderUpdate(_ctx, ev) { events.push(ev); },
    onFill(_ctx, ev) { events.push(ev); },
  });
  const N = 6;
  const starts = new BigInt64Array(N);
  const ends = new BigInt64Array(N);
  const lows = new Float64Array(N).fill(99.5);
  for (let i = 0; i < N; i++) {
    starts[i] = T0 + SEC * BigInt(i);
    ends[i] = T0 + SEC * BigInt(i + 1);
  }
  lows[2] = 98;
  bt.runBars(starts, ends, new Float64Array(N).fill(100), new Float64Array(N).fill(100.5), lows,
             new Float64Array(N).fill(100), new Float64Array(N).fill(10), 'AAA');

  const FIELDS = ['exchangeTsNs', 'submittedAtNs', 'acceptedAtNs', 'firstFillAtNs',
                  'lastFillAtNs', 'canceledAtNs', 'rejectedAtNs', 'triggeredAtNs', 'expiredAtNs'];
  check(events.length > 0, `the strategy received order events (${events.length})`);
  for (const f of FIELDS) {
    check(events.length > 0 && events.every((e) => typeof e[f] === 'bigint'),
          `order event ${f} is a bigint (${typeof (events[0] || {})[f]})`);
  }
  const barEnds = new Set(Array.from(ends, String));
  const filled = events.find((e) => e.status === 'FILLED');
  check(filled !== undefined, 'the resting buy filled');
  if (filled !== undefined && typeof filled.exchangeTsNs === 'bigint') {
    check(barEnds.has(String(filled.exchangeTsNs)) || barEnds.has(String(filled.exchangeTsNs - 1n)) ||
          Array.from(starts, String).includes(String(filled.exchangeTsNs)),
          `the fill's exchangeTsNs is a bar boundary fed in (${filled.exchangeTsNs})`);
    check(filled.exchangeTsNs % 256n !== 0n, 'and is not rounded to the double step');
    check(filled.submittedAtNs > 0n && filled.firstFillAtNs >= filled.submittedAtNs,
          'lifecycle timestamps order against each other');
  }
  check(typeof (events[0] || {}).orderId === 'number', 'orderId stays a number');

  const rows = tracer.result();
  check(rows.length > 0, `the tracer recorded rows (${rows.length})`);
  for (const f of ['tsNs', 'submittedAtNs', 'acceptedAtNs', 'firstFillAtNs', 'lastFillAtNs',
                   'canceledAtNs', 'rejectedAtNs', 'triggeredAtNs', 'expiredAtNs']) {
    check(rows.length > 0 && rows.every((r) => typeof r[f] === 'bigint'),
          `tracer row ${f} is a bigint (${typeof (rows[0] || {})[f]})`);
  }
  check(rows.some((r) => typeof r.tsNs === 'bigint' && r.tsNs % 256n !== 0n), 'a tracer tsNs a double could not hold arrives intact');
  check(typeof tracer.medianAckLatencyNs() === 'number', 'medianAckLatencyNs stays a number');
}

console.log('=== execution algos ===');
{
  const twap = new lrvx.TWAPExecutor({
    targetQty: 4, side: 'buy', durationNs: 4e9, sliceCount: 4, startTimeNs: T0,
  });
  const children = twap.step(T0 + 1n);
  check(children.length > 0, `TWAP emitted a child at its start (${children.length})`);
  exact((children[0] || {}).timestampNs, T0 + 1n, 'child.timestampNs is the step clock');
}

console.log('=== RateLimitPolicy / FeeSchedule ===');
{
  const policy = new lrvx.RateLimitPolicy();
  exact(policy.banUntilNs(), 0n, 'banUntilNs() with no ban');
  check(Array.isArray(policy.bucketStates(T0)), 'bucketStates takes a bigint clock');

  const fees = new lrvx.FeeSchedule();
  fees.addTier(0, 2, 5);
  fees.addTier(1000, 1, 4);
  fees.recordFill(T0, 10);
  fees.recordFill(T0 + 3n, 5000);
  const transitions = fees.tierTransitions();
  check(transitions.length === 1, `one tier transition (${transitions.length})`);
  exact(transitions[0], T0 + 3n, 'tierTransitions()[0]');
  check(typeof fees.feeFor(T0 + 5n, 100, true) === 'number', 'feeFor takes a bigint clock');
}

console.log('=== TraceReader ===');
{
  const out = path.join(root, 'run.lrvxrun');
  const rec = new lrvx.TraceRecorder({path: out, strategyId: 's', runStartedNs: T0});
  rec.addTapeRef({path: 'a.lrvx', firstEventNs: T0, lastEventNs: T0 + 9n});
  rec.writeSignal({runTsNs: T0 + 1n, feedTsNs: T0 + 2n, signalId: 1, name: 'n',
                   symbolIds: [1], payload: Buffer.alloc(0)});
  rec.writeOrderEvent({runTsNs: T0 + 3n, feedTsNs: T0 + 4n, orderId: 7, symbolId: 1,
                       eventKind: 1, side: 0, orderType: 1, flags: 0, reason: ''});
  rec.writeFill({runTsNs: T0 + 5n, feedTsNs: T0 + 6n, orderId: 7, fillId: 1, symbolId: 1,
                 side: 0, liquidity: 1});
  rec.setRunEndedNs(T0 + 7n);
  rec.close();

  const reader = new lrvx.TraceReader(out);
  exact(reader.runStartedNs(), T0, 'runStartedNs()');
  exact(reader.runEndedNs(), T0 + 7n, 'runEndedNs()');
  const sig = reader.readAllSignals()[0] || {};
  exact(sig.runTsNs, T0 + 1n, 'signal.runTsNs');
  exact(sig.feedTsNs, T0 + 2n, 'signal.feedTsNs');
  const ord = reader.readAllOrderEvents()[0] || {};
  exact(ord.runTsNs, T0 + 3n, 'orderEvent.runTsNs');
  exact(ord.feedTsNs, T0 + 4n, 'orderEvent.feedTsNs');
  const fill = reader.readAllFills()[0] || {};
  exact(fill.runTsNs, T0 + 5n, 'fill.runTsNs');
  exact(fill.feedTsNs, T0 + 6n, 'fill.feedTsNs');
  reader.close();

  // A plain number is still accepted where it was before.
  const out2 = path.join(root, 'run2.lrvxrun');
  const rec2 = new lrvx.TraceRecorder({path: out2, strategyId: 's', runStartedNs: 1000});
  rec2.writeSignal({runTsNs: 2000, name: 'n', symbolIds: [1], payload: Buffer.alloc(0)});
  rec2.setRunEndedNs(3000);
  rec2.close();
  const reader2 = new lrvx.TraceReader(out2);
  exact(reader2.runStartedNs(), 1000n, 'runStartedNs() written as a number');
  exact(reader2.runEndedNs(), 3000n, 'runEndedNs() written as a number');
  reader2.close();
}

console.log('=== tapeDiff ===');
{
  const mk = (name, price) => {
    const dir = path.join(root, name);
    fs.mkdirSync(dir);
    const w = new lrvx.DataWriter(dir, 1, 0);
    w.writeTrade(T0, T0, 100, 1, 1, 1, 0);
    w.writeTrade(T0 + 1n, T0 + 1n, price, 1, 2, 1, 1);
    w.flush();
    w.close();
    return dir;
  };
  const r = lrvx.tapeDiff(mk('left', 101), mk('right', 999));
  check(r.mismatches.length === 1, `one mismatch (${r.mismatches.length})`);
  const m = r.mismatches[0] || {left: {}, right: {}};
  exact(m.left.exchangeTsNs, T0 + 1n, 'mismatch.left.exchangeTsNs');
  exact(m.right.exchangeTsNs, T0 + 1n, 'mismatch.right.exchangeTsNs');
}

fs.rmSync(root, {recursive: true, force: true});

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed > 0 ? 1 : 0);
