// node/test/test_backtest_run_tape_bars.js
//
// BacktestRunner.runTapeBars(path, intervalSeconds, symbol) and the
// slippage / latency options of the BacktestRunner constructor. Mirrors
// python/tests/test_run_tape_bars.py and
// python/tests/test_runner_slippage_latency.py:
//   * runTapeBars equals DataReader.readTrades -> aggregateTimeBars ->
//     runBars for the same interval, bar for bar and trade for trade
//   * a multi-symbol tape needs a symbol; bad intervals throw
//   * options reach the executor (bps slippage, submit latency, a latency
//     model) and invalid ones throw LrvxError(E_VAL_002)

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

// Two symbols, three segments written latest-first.
function writeTape(dir)
{
  let seed = 7;
  const rnd = () => (seed = (seed * 1103515245 + 12345) % 2147483648) / 2147483648;
  for (const k of [2, 0, 1])
  {
    const w = new lrvx.DataWriter(dir);
    const rows = [];
    for (const [sym, base, n] of [[1, 100 + k, 600], [2, 50 + k, 400]])
    {
      let t = T0 + BigInt(k) * 3600n * SEC;
      let px = Math.round(base * 100);
      for (let i = 0; i < n; i++)
      {
        t += BigInt(1 + Math.floor(rnd() * 5000)) * 1000000n;
        px += Math.floor(rnd() * 51) - 25;
        rows.push([t, px / 100, Math.round(1 + rnd() * 1999) / 1000, sym, i % 3 === 0 ? 0 : 1]);
      }
    }
    rows.sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0));
    rows.forEach((r, i) => w.writeTrade(r[0], r[0], r[1], r[2], i + 1, r[3], r[4]));
    w.close();
  }
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
  const s = {
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
  return s;
}

function oneCall(tape, symId, name, interval, options)
{
  const bt = new lrvx.BacktestRunner(registry(), 0.0004, 10000, options);
  const s = strategy(symId);
  bt.setStrategy(s);
  const stats = bt.runTapeBars(tape, interval, name);
  return {stats, trades: bt.trades(), equity: bt.equityCurve(), bars: s.bars};
}

function threeStep(tape, symId, name, interval)
{
  const trades = new lrvx.DataReader(tape).readTrades().filter((t) => t.symbolId === symId);
  const ts = BigInt64Array.from(trades.map((t) => BigInt(t.exchangeTsNs)));
  const bars = lrvx.aggregateTimeBars(ts, Float64Array.from(trades.map((t) => t.price)),
                                      Float64Array.from(trades.map((t) => t.qty)),
                                      Uint8Array.from(trades.map((t) => (t.side === 0 || t.side === 'buy') ? 1 : 0)),
                                      interval);
  const bt = new lrvx.BacktestRunner(registry(), 0.0004, 10000);
  const s = strategy(symId);
  bt.setStrategy(s);
  const stats = bt.runBars(
      BigInt64Array.from(bars.map((b) => BigInt(b.startTimeNs))),
      BigInt64Array.from(bars.map((b) => BigInt(b.endTimeNs))),
      Float64Array.from(bars.map((b) => b.open)), Float64Array.from(bars.map((b) => b.high)),
      Float64Array.from(bars.map((b) => b.low)), Float64Array.from(bars.map((b) => b.close)),
      Float64Array.from(bars.map((b) => b.volume)), name, 0, interval * 1e9);
  return {stats, trades: bt.trades(), equity: bt.equityCurve(), bars: s.bars, n: bars.length};
}

const same = (a, b) => JSON.stringify(a, (_k, v) => (typeof v === 'bigint' ? v.toString() : v)) ===
                       JSON.stringify(b, (_k, v) => (typeof v === 'bigint' ? v.toString() : v));

function main()
{
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'lrvx-tape-bars-'));
  const tape = path.join(dir, 'tape');
  fs.mkdirSync(tape);
  writeTape(tape);

  console.log('test_run_tape_bars_equals_three_step_route');
  for (const interval of [60, 300])
  {
    for (const [symId, name] of [[1, 'AAA'], [2, 'BBB']])
    {
      const a = oneCall(tape, symId, name, interval);
      const b = threeStep(tape, symId, name, interval);
      const tag = `${name} ${interval}s`;
      check(b.n > 10 && a.bars.length === b.n, `${tag}: on_bar count equals the bar count (${b.n})`);
      check(same(a.stats, b.stats), `${tag}: stats equal`);
      check(a.trades.entryPrice.length >= 2 && same(a.trades, b.trades), `${tag}: trades equal`);
      check(same(a.equity, b.equity), `${tag}: equity curve equal`);
      check(same(a.bars, b.bars), `${tag}: bars equal`);
    }
  }

  console.log('test_run_tape_bars_errors');
  const bt = new lrvx.BacktestRunner(registry(), 0.0004, 10000);
  bt.setStrategy(strategy(1));
  check(throwsWith(() => bt.runTapeBars(tape, 60), /more than one symbol/),
        'multi-symbol tape without a symbol throws');
  check(throwsWith(() => bt.runTapeBars(tape, 0, 'AAA'), /intervalSeconds/), 'interval 0 throws');
  check(throwsWith(() => bt.runTapeBars(tape, NaN, 'AAA'), /intervalSeconds/), 'NaN interval throws');
  check(throwsWith(() => bt.runTapeBars(tape, 60, 'ZZZ'), /ZZZ/), 'unknown symbol throws');

  console.log('test_runner_options');
  const plain = oneCall(tape, 1, 'AAA', 60);
  const zero = oneCall(tape, 1, 'AAA', 60, {slippageModel: 'none', submitAckLatencyNs: 0});
  check(same(plain.trades, zero.trades) && same(plain.stats, zero.stats), 'default options equal no options');
  const bps = oneCall(tape, 1, 'AAA', 60, {slippageModel: 'fixed_bps', slippageBps: 10});
  const n = plain.trades.entryPrice.length;
  check(bps.trades.entryPrice.length === n && n > 0 &&
            Array.from(bps.trades.entryPrice).every(
                (p, i) => Math.abs(p - plain.trades.entryPrice[i] * 1.001) < 1e-6 &&
                          Math.abs(bps.trades.exitPrice[i] - plain.trades.exitPrice[i] * 0.999) < 1e-6),
        'fixed_bps moves every fill 10 bps against the taker');
  const acked = oneCall(tape, 1, 'AAA', 60, {submitAckLatencyNs: 5000000});
  const modelled = oneCall(tape, 1, 'AAA', 60,
                           {latency: new lrvx.ConstantLatency({feedNs: 0, orderNs: 5000000, fillNs: 0})});
  check(same(acked.trades, modelled.trades), 'a latency model composes like the submit ack latency');
  check(acked.trades.entryPrice[0] === acked.bars[2][2] && plain.trades.entryPrice[0] === plain.bars[1][2],
        '5 ms submit latency moves the first fill one bar later on bar replay');

  const reg = registry();
  check(throwsWith(() => new lrvx.BacktestRunner(reg, 0, 1000, {slippageModel: 'fixed_bps'}), /slippageBps/),
        'fixed_bps without slippageBps throws');
  check(throwsWith(() => new lrvx.BacktestRunner(reg, 0, 1000, {slippageModel: 'bogus'}), /slippageModel/),
        'unknown model throws');
  check(throwsWith(() => new lrvx.BacktestRunner(reg, 0, 1000, {slippageBps: 2}), /slippageBps/),
        'a parameter the model does not read throws');
  check(throwsWith(() => new lrvx.BacktestRunner(reg, 0, 1000, {submitAckLatencyNs: -1}), /submitAckLatencyNs/),
        'negative latency throws');
  check(throwsWith(() => new lrvx.BacktestRunner(reg, 0, 1000, {slipageBps: 2}), /unknown option/),
        'a misspelt option throws');
  const vi = new lrvx.BacktestRunner(reg, 0, 1000, {slippageModel: 'volume_impact', slippageImpactCoeff: 0.5});
  vi.setStrategy(strategy(1));
  check(throwsWith(() => vi.runTapeBars(tape, 60, 'AAA'), /volume_impact/), 'volume_impact is refused on bars');

  fs.rmSync(dir, {recursive: true, force: true});
  console.log(`\n${_passed} passed, ${_failed} failed`);
  process.exit(_failed === 0 ? 0 : 1);
}

main();
