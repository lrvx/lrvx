// node/test/test_merged_tape_reader.js
//
// N-API binding for MergedTapeReader. Mirrors the Python
// suite in python/tests/test_merged_tape_reader.py:
//   * single tape ≡ DataReader on trade fields
//   * two non-overlapping tapes → merged count + time-sorted
//   * cross-exchange same symbol → distinct globalIds
//   * overlapping book streams → constructor throws
//   * replay-style time order across tapes

'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');
const lrvx = require('..');

let _failed = 0;
let _passed = 0;
function check(cond, msg)
{
  if (cond)
  {
    _passed++;
    console.log(`  ok  - ${msg}`);
  }
  else
  {
    _failed++;
    console.log(`  FAIL - ${msg}`);
  }
}

function mkTmpDir(label)
{
  return fs.mkdtempSync(path.join(os.tmpdir(), `lrvx-mtr-${label}-`));
}

// Write one .lrvx tape via the runtime path: SymbolRegistry +
// Runner + BinaryLogRecorderHook. Mirrors the Python `_write_tape`
// fixture so the test surface is identical across languages.
function writeTape(outDir, exchange, symbolName, trades, exchangeId, books)
{
  const reg = new lrvx.SymbolRegistry();
  const sym = reg.addSymbol(exchange, symbolName, 0.01);
  const hook = new lrvx.BinaryLogRecorderHook(
      outDir, 4, exchangeId, 'none', exchange, 'perpetual');
  hook.addSymbol(Number(sym), symbolName, '', '', 2, 6);
  const runner = new lrvx.Runner(reg, () => {}, false);
  runner.addStrategy({symbols: [sym]});
  runner.setMarketDataRecorder(hook);
  runner.start();
  for (const [tsNs, price, qty, isBuy] of trades)
  {
    // RunnerNode::onTrade accepts BigInt for the timestamp (as well as
    // Number, for callers whose values fit under 2^53) so a real ns
    // timestamp round-trips losslessly. Pass the BigInt straight through.
    runner.onTrade(Number(sym), price, qty, isBuy, tsNs);
  }
  for (const entry of (books || []))
  {
    const [tsNs, bidP, bidQ, askP, askQ] = entry;
    // onBookSnapshot takes Float64Array; fixture entries above are
    // plain array literals for readability.
    runner.onBookSnapshot(
        Number(sym), Float64Array.from(bidP), Float64Array.from(bidQ),
        Float64Array.from(askP), Float64Array.from(askQ), tsNs);
  }
  runner.stop();
  hook.flush();
}

// ── 1. single tape ≡ DataReader ──────────────────────────────────────

function testSingleTapeRoundTrip()
{
  console.log('test_single_tape_round_trip');
  const d = mkTmpDir('single');
  const tape = path.join(d, 'bybit');
  // A real exchange nanosecond timestamp (~1.7e18), not a value that
  // happens to survive a double -- runner.onTrade now takes the ts
  // straight through to a bigint-aware conversion, so precision must
  // survive the whole write/read round trip through both readers.
  const base = 1_765_615_835_519_000_000n;
  const trades = [];
  for (let i = 0; i < 5; i++)
  {
    trades.push([base + BigInt(i) * 1_000_000n, 50000.0 + i, 0.1, i % 2 === 0]);
  }
  writeTape(tape, 'bybit', 'BTCUSDT', trades, 0);

  const dr = new lrvx.DataReader(tape);
  const mr = new lrvx.MergedTapeReader([tape]);
  const single = dr.readTrades();
  const merged = mr.readTrades();

  check(single.length === merged.length,
        `length matches (${single.length} == ${merged.length})`);
  let priceOk = true;
  let qtyOk = true;
  let sideOk = true;
  let tsTypeOk = true;
  let tsValueOk = true;
  for (let i = 0; i < single.length; i++)
  {
    if (single[i].price !== merged[i].price) { priceOk = false; }
    if (single[i].qty !== merged[i].qty) { qtyOk = false; }
    if (single[i].side !== merged[i].side) { sideOk = false; }
    // TradeRecord.exchangeTsNs is declared `bigint` for both DataReader
    // and MergedTapeReader. When the two disagreed (number vs bigint),
    // `single[i].exchangeTsNs === merged[i].exchangeTsNs` was always
    // false and arithmetic between the two readers threw.
    if (typeof single[i].exchangeTsNs !== 'bigint' || typeof merged[i].exchangeTsNs !== 'bigint')
    {
      tsTypeOk = false;
    }
    if (single[i].exchangeTsNs !== merged[i].exchangeTsNs) { tsValueOk = false; }
  }
  check(priceOk, 'price preserved across rekey');
  check(qtyOk, 'qty preserved across rekey');
  check(sideOk, 'side preserved across rekey');
  check(typeof single[0]?.side === 'string', `side is a string (got ${typeof single[0]?.side})`);
  check(tsTypeOk, 'exchangeTsNs is bigint on both readers');
  check(tsValueOk, 'exchangeTsNs matches exactly between DataReader and MergedTapeReader');
  check(single[0]?.exchangeTsNs === BigInt(base),
        `DataReader exchangeTsNs survives a real ns timestamp (${single[0]?.exchangeTsNs} == ${base})`);

  const tbl = mr.symbolTable();
  check(tbl.length === 1, `symbolTable has one entry (got ${tbl.length})`);
  check(tbl[0].globalId === 1,
        `single-tape single-symbol globalId is 1 (got ${tbl[0].globalId})`);
  check(tbl[0].exchange === 'bybit',
        `symbolTable.exchange='bybit' (got '${tbl[0].exchange}')`);
  check(tbl[0].name === 'BTCUSDT',
        `symbolTable.name='BTCUSDT' (got '${tbl[0].name}')`);
  // Every merged trade reports the rekeyed global id.
  let allRekeyed = true;
  for (const t of merged)
  {
    if (t.symbolId !== 1) { allRekeyed = false; break; }
  }
  check(allRekeyed, 'every merged trade has symbolId == globalId (1)');

  fs.rmSync(d, {recursive: true, force: true});
}

// ── 2. two non-overlapping tapes ─────────────────────────────────────

function testTwoTapesNonOverlapping()
{
  console.log('test_two_tapes_non_overlapping');
  const d = mkTmpDir('twonon');
  const t1 = path.join(d, 'bybit');
  const t2 = path.join(d, 'binance');
  // Keep below 2^53 so runner.onTrade's Number path doesn't truncate.
  const base = 1_000_000_000_000n;
  const tradesA = [];
  for (let i = 0; i < 5; i++)
  {
    tradesA.push([base + BigInt(i) * 1_000_000n, 50000.0 + i, 0.1, true]);
  }
  const tradesB = [];
  for (let i = 0; i < 3; i++)
  {
    tradesB.push(
        [base + 500_000n + BigInt(i) * 1_000_000n, 3000.0 + i, 1.0, false]);
  }
  writeTape(t1, 'bybit', 'BTCUSDT', tradesA, 1);
  writeTape(t2, 'binance', 'ETHUSDT', tradesB, 2);

  const mr = new lrvx.MergedTapeReader([t1, t2]);
  const trades = mr.readTrades();
  check(trades.length === 8, `merged trade count is 8 (got ${trades.length})`);

  // exchange_ts is non-decreasing across the merged stream.
  let sorted = true;
  for (let i = 1; i < trades.length; i++)
  {
    if (trades[i].exchangeTsNs < trades[i - 1].exchangeTsNs)
    {
      sorted = false;
      break;
    }
  }
  check(sorted, 'merged trades are time-sorted across tapes');

  // Both symbols present with distinct global IDs.
  const idSet = new Set(trades.map((t) => t.symbolId));
  const ids = [...idSet].sort();
  check(ids.length === 2 && ids[0] === 1 && ids[1] === 2,
        `two distinct global IDs [1, 2] (got [${ids.join(', ')}])`);

  // per-tape stats: two entries, sorted as the paths arg.
  const stats = mr.perTapeStats();
  check(stats.length === 2,
        `perTapeStats has 2 entries (got ${stats.length})`);
  check(stats[0].trades === 5n && stats[1].trades === 3n,
        `per-tape trade counts [5, 3] (got [${stats[0].trades}, ` +
            `${stats[1].trades}])`);

  // timeRange returns BigInts spanning the union of both tapes.
  const tr = mr.timeRange();
  check(Array.isArray(tr) && tr.length === 2,
        'timeRange returns [bigint, bigint]');
  check(typeof tr[0] === 'bigint' && typeof tr[1] === 'bigint',
        'timeRange entries are bigint');
  check(tr[0] === base && tr[1] >= base,
        'timeRange covers the merged span');

  fs.rmSync(d, {recursive: true, force: true});
}

// ── 3. cross-exchange same symbol → separate globalIds ───────────────

function testCrossExchangeSameSymbol()
{
  console.log('test_cross_exchange_same_symbol_separate_global_ids');
  const d = mkTmpDir('xexch');
  const t1 = path.join(d, 'bybit');
  const t2 = path.join(d, 'binance');
  // Keep below 2^53 so runner.onTrade's Number path doesn't truncate.
  const base = 1_000_000_000_000n;
  const tA = [];
  for (let i = 0; i < 2; i++)
  {
    tA.push([base + BigInt(i) * 1_000_000n, 50000.0, 0.1, true]);
  }
  const tB = [];
  for (let i = 0; i < 2; i++)
  {
    tB.push(
        [base + 500_000n + BigInt(i) * 1_000_000n, 50100.0, 0.1, true]);
  }
  writeTape(t1, 'bybit', 'BTCUSDT', tA, 1);
  writeTape(t2, 'binance', 'BTCUSDT', tB, 2);

  const mr = new lrvx.MergedTapeReader([t1, t2]);
  const tbl = mr.symbolTable();
  check(tbl.length === 2,
        `symbolTable has 2 entries (got ${tbl.length})`);
  const exchanges = new Set(tbl.map((s) => s.exchange));
  check(exchanges.has('bybit') && exchanges.has('binance'),
        'both exchanges present');
  const gids = tbl.map((s) => s.globalId).sort();
  check(gids[0] === 1 && gids[1] === 2,
        `globalIds are [1, 2] (got [${gids.join(', ')}])`);

  fs.rmSync(d, {recursive: true, force: true});
}

// ── 4. overlapping book streams throw ────────────────────────────────

function testOverlappingBookStreamsRaise()
{
  console.log('test_overlapping_book_streams_raise');
  const d = mkTmpDir('overlap');
  const t1 = path.join(d, 'session-a');
  const t2 = path.join(d, 'session-b');
  // Keep below 2^53 so runner.onTrade's Number path doesn't truncate.
  const base = 1_000_000_000_000n;
  writeTape(t1, 'bybit', 'BTCUSDT',
            [[base, 50000.0, 0.1, true]],
            1,
            [[base + 100_000n, [49999.0], [1.0], [50001.0], [1.0]]]);
  writeTape(t2, 'bybit', 'BTCUSDT',
            [[base + 50_000n, 50001.0, 0.1, true]],
            1,
            [[base + 200_000n, [49998.0], [2.0], [50002.0], [2.0]]]);

  let threw = false;
  let msg = '';
  try
  {
    new lrvx.MergedTapeReader([t1, t2]);
  }
  catch (e)
  {
    threw = true;
    msg = String(e && e.message ? e.message : e);
  }
  check(threw, 'overlapping book streams: constructor throws');
  check(msg.toLowerCase().includes('overlapping'),
        `error message mentions overlapping (got: "${msg}")`);

  fs.rmSync(d, {recursive: true, force: true});
}

// ── 5. replay-style time order across tapes ──────────────────────────

function testReplayTimeOrderAcrossTapes()
{
  console.log('test_replay_time_order_across_tapes');
  const d = mkTmpDir('replay');
  const t1 = path.join(d, 'a');
  const t2 = path.join(d, 'b');
  // Keep below 2^53 so runner.onTrade's Number path doesn't truncate.
  const base = 1_000_000_000_000n;
  const tA = [];
  for (let i = 0; i < 3; i++)
  {
    tA.push([base + BigInt(i) * 2_000_000n, 100.0 + i, 1.0, true]);
  }
  const tB = [];
  for (let i = 0; i < 3; i++)
  {
    tB.push(
        [base + 1_000_000n + BigInt(i) * 2_000_000n, 200.0 + i, 2.0, false]);
  }
  writeTape(t1, 'ex_a', 'FOO', tA, 1);
  writeTape(t2, 'ex_b', 'FOO', tB, 2);

  const mr = new lrvx.MergedTapeReader([t1, t2]);
  const trades = mr.readTrades();
  check(trades.length === 6, `total 6 trades (got ${trades.length})`);

  // Non-decreasing by exchange_ts.
  let sorted = true;
  for (let i = 1; i < trades.length; i++)
  {
    if (trades[i].exchangeTsNs < trades[i - 1].exchangeTsNs)
    {
      sorted = false;
      break;
    }
  }
  check(sorted, 'time-sorted across tapes');

  // Two tapes interleave 1, 2, 1, 2, 1, 2 (each carries a distinct symbol
  // because their `(exchange, name)` keys differ — same name but
  // different exchange).
  const symbols = trades.map((t) => t.symbolId);
  check(symbols.join(',') === '1,2,1,2,1,2',
        `symbols alternate 1,2,1,2,1,2 (got ${symbols.join(',')})`);

  fs.rmSync(d, {recursive: true, force: true});
}

// ── driver ───────────────────────────────────────────────────────────

testSingleTapeRoundTrip();
testTwoTapesNonOverlapping();
testCrossExchangeSameSymbol();
testOverlappingBookStreamsRaise();
testReplayTimeOrderAcrossTapes();

console.log(`\n${_passed} passed, ${_failed} failed`);
if (_failed > 0)
{
  process.exit(1);
}
