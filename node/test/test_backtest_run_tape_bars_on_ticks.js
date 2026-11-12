// node/test/test_backtest_run_tape_bars_on_ticks.js
//
// BacktestRunner.runTapeBarsOnTicks(path, intervalSeconds, symbol): onBar sees
// the bars runTapeBars delivers, the executor matches on the tape's prints.
//   * the bars are runTapeBars' bars
//   * an order sent from bar n's onBar fills at the first print of the symbol
//     stamped at or after the end of bar n, and a 5 ms submit latency moves
//     it by milliseconds, not by a bar
//   * every run's stats carry skippedOptionQuotes / skippedPoolStates
//   * a multi-symbol tape needs a symbol; bad intervals throw

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

function throwsWith(fn, re)
{
  try
  {
    fn();
  }
  catch (e)
  {
    return re.test(String(e.message));
  }
  return false;
}

const SEC = 1000000000n;
const T0 = 1767225600n * SEC;  // 2026-01-01T00:00:00Z

// Returns symbol 1's prints, [ts (BigInt), price], in time order. Kept from the
// writer rather than read back: readTrades hands timestamps out as Numbers,
// which do not hold a nanosecond Unix time exactly.
function writeTape(dir)
{
  const prints = [];
  let seed = 11;
  const rnd = () => (seed = (seed * 1103515245 + 12345) % 2147483648) / 2147483648;
  for (const k of [1, 0])
  {
    const w = new lrvx.DataWriter(dir);
    const rows = [];
    for (const [sym, base, n] of [[1, 100 + k, 900], [2, 50 + k, 300]])
    {
      let t = T0 + BigInt(k) * 3600n * SEC;
      let px = Math.round(base * 100);
      for (let i = 0; i < n; i++)
      {
        t += BigInt(1 + Math.floor(rnd() * 4000)) * 1000000n;
        px += Math.floor(rnd() * 41) - 20;
        rows.push([t, px / 100, Math.round(1 + rnd() * 999) / 1000, sym, i % 3 === 0 ? 0 : 1]);
      }
    }
    rows.sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0));
    rows.forEach((r, i) => w.writeTrade(r[0], r[0], r[1], r[2], i + 1, r[3], r[4]));
    w.close();
    rows.filter((r) => r[3] === 1).forEach((r) => prints.push([r[0], r[1]]));
  }
  prints.sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0));
  return prints;
}

function registry()
{
  const reg = new lrvx.SymbolRegistry();
  reg.addSymbol('test', 'AAA', 0.01);
  reg.addSymbol('test', 'BBB', 0.01);
  return reg;
}

function strategy(symId)
{
  const bars = [];
  return {
    symbols: [symId],
    bars,
    onBar(_ctx, bar, emit)
    {
      bars.push([bar.startTimeNs, bar.endTimeNs, bar.open, bar.high, bar.low, bar.close]);
      const i = bars.length;
      if (i % 6 === 1)
      {
        emit.marketBuy(0.5, symId);
      }
      else if (i % 6 === 4)
      {
        emit.marketSell(0.5, symId);
      }
    },
  };
}

function run(tape, method, options)
{
  const bt = new lrvx.BacktestRunner(registry(), 0.0004, 10000, options);
  const s = strategy(1);
  bt.setStrategy(s);
  const stats = bt[method](tape, 60, 'AAA');
  return {stats, trades: bt.trades(), equity: bt.equityCurve(), bars: s.bars};
}

const same = (a, b) => JSON.stringify(a, (_k, v) => (typeof v === 'bigint' ? v.toString() : v)) ===
                       JSON.stringify(b, (_k, v) => (typeof v === 'bigint' ? v.toString() : v));

function main()
{
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'lrvx-tape-bars-ticks-'));
  const tape = path.join(dir, 'tape');
  fs.mkdirSync(tape);
  const prints = writeTape(tape);
  const firstAtOrAfter = (ns) =>
  {
    const p = prints.find((t) => t[0] >= BigInt(ns));
    return {price: p[1], exchangeTsNs: p[0]};
  };

  console.log('test_bars_on_ticks');
  const onBars = run(tape, 'runTapeBars');
  const onTicks = run(tape, 'runTapeBarsOnTicks');
  check(onBars.bars.length > 50 && same(onTicks.bars, onBars.bars), 'onBar sees the runTapeBars bars');
  const entry = firstAtOrAfter(onTicks.bars[0][1]);
  check(onTicks.trades.entryPrice.length > 2 && onTicks.trades.entryPrice[0] === entry.price &&
            BigInt(onTicks.trades.entryTimeNs[0]) === BigInt(entry.exchangeTsNs),
        'the first entry fills at the first print at or after the end of its bar');
  check(onBars.trades.entryPrice[0] === onBars.bars[1][2], 'bar replay fills it at the next open');
  check(typeof onTicks.stats.netPnl === 'number' && onTicks.equity.equity.length > 0,
        'result shape as the other runners');

  const late = run(tape, 'runTapeBarsOnTicks', {submitAckLatencyNs: 5000000});
  const end0 = BigInt(onTicks.bars[0][1]);
  const lateEntry = firstAtOrAfter(end0 + 5000000n);
  const lateTs = BigInt(late.trades.entryTimeNs[0]);
  check(late.trades.entryPrice[0] === lateEntry.price && lateTs === BigInt(lateEntry.exchangeTsNs) &&
            lateTs < BigInt(onTicks.bars[1][1]),
        '5 ms submit latency moves the fill to the first print at end + 5 ms, inside the next bar');

  console.log('test_skipped_record_counts');
  check(onTicks.stats.skippedOptionQuotes === 0 && onTicks.stats.skippedPoolStates === 0,
        'runTapeBarsOnTicks reports the skipped record counts');
  check(onBars.stats.skippedOptionQuotes === 0 && onBars.stats.skippedPoolStates === 0,
        'runTapeBars reports them too');
  const bt = new lrvx.BacktestRunner(registry(), 0.0004, 10000);
  bt.setStrategy(strategy(1));
  const tapeStats = bt.runTape(tape);
  check(tapeStats.skippedOptionQuotes === 0 && tapeStats.skippedPoolStates === 0,
        'runTape reports them');

  console.log('test_errors');
  check(throwsWith(() => bt.runTapeBarsOnTicks(tape, 60), /more than one symbol/),
        'multi-symbol tape without a symbol throws');
  check(throwsWith(() => bt.runTapeBarsOnTicks(tape, 0, 'AAA'), /intervalSeconds/), 'interval 0 throws');
  check(throwsWith(() => bt.runTapeBarsOnTicks(tape, NaN, 'AAA'), /intervalSeconds/),
        'NaN interval throws');
  check(throwsWith(() => bt.runTapeBarsOnTicks(tape, 60, 'ZZZ'), /ZZZ/), 'unknown symbol throws');

  fs.rmSync(dir, {recursive: true, force: true});
  console.log(`\n${_passed} passed, ${_failed} failed`);
  process.exit(_failed === 0 ? 0 : 1);
}

main();
