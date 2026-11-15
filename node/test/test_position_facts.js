'use strict';
/**
 * node/test/test_position_facts.js -- ctx.entryPrice and ctx.barsInPosition.
 *
 * entryPrice is the average entry price of the open position: adds weight it
 * by quantity, a partial close leaves it, a fill that flips the position sets
 * it to that fill's price, and it is null when flat. barsInPosition counts the
 * bar callbacks received while holding the current position, the bar on which
 * the position is first seen being 1; 0 when flat, 1 again after a flip, and
 * 0 throughout a run without bars.
 *
 * Run from repo root:
 *   cd node && node test/test_position_facts.js
 */

const path = require('path');
const lrvx = require(path.join(__dirname, '..'));

let passed = 0;
let failed = 0;
function check(cond, msg) {
  if (cond) { passed++; console.log(`  ok  ${msg}`); }
  else { failed++; console.error(`  FAIL  ${msg}`); }
}
const near = (a, b) => typeof a === 'number' && Math.abs(a - b) < 1e-9;

const MIN = 60000000000n;

// Bars i = 0..n-1: open 100 + 10 i, high open + 3, low open - 2, close open + 1.
// Every field differs, so a fill taken at the wrong one shows.
function bars(n) {
  const st = new BigInt64Array(n);
  const en = new BigInt64Array(n);
  const o = new Float64Array(n);
  for (let i = 0; i < n; i++) {
    st[i] = BigInt(i) * MIN;
    en[i] = st[i] + MIN;
    o[i] = 100 + 10 * i;
  }
  return [st, en, o, o.map((x) => x + 3), o.map((x) => x - 2), o.map((x) => x + 1),
          new Float64Array(n).fill(100)];
}

// An order sent from onBar fills at the next bar's open, before that bar's
// onBar: buy 1 @120, add 1 @140 (average 130), sell 1 @160 (partial close,
// average stays 130; first-in-first-out would read 140), sell 3 @180 (flip
// to -2 at 180), buy 2 @200 (flat).
const ACTIONS = {1: ['buy', 1], 3: ['buy', 1], 5: ['sell', 1], 7: ['sell', 3], 9: ['buy', 2]};
const EXPECTED = [
  [null, 0], [null, 0], [120, 1], [120, 2], [130, 3], [130, 4], [130, 5], [130, 6],
  [180, 1], [180, 2], [null, 0], [null, 0],
];

function scripted(sym, log, actions, state = {n: 0}) {
  return {
    symbols: [sym],
    onBar(ctx, _bar, emit) {
      const n = state.n;
      log.push([ctx.entryPrice, ctx.barsInPosition, ctx.position]);
      const a = actions[n];
      if (a) {
        if (a[0] === 'buy') { emit.marketBuy(a[1]); } else { emit.marketSell(a[1]); }
      }
      state.n += 1;
    },
  };
}

function expectScenario(log, label) {
  check(log.length === EXPECTED.length, `${label}: ${EXPECTED.length} bars (${log.length})`);
  EXPECTED.forEach(([entry, held], i) => {
    const got = log[i] || [];
    const entryOk = entry === null ? got[0] === null : near(got[0], entry);
    check(entryOk, `${label}: bar ${i} entryPrice ${got[0]}, want ${entry}`);
    check(got[1] === held, `${label}: bar ${i} barsInPosition ${got[1]}, want ${held}`);
  });
}

console.log('=== run_bars: entry, add, partial close, flip, flat ===');
{
  const reg = new lrvx.SymbolRegistry();
  const sym = reg.addSymbol('sim', 'BTCUSDT', 0.01);
  const bt = new lrvx.BacktestRunner(reg, 0, 1e6);
  const log = [];
  bt.setStrategy(scripted(sym, log, ACTIONS));
  bt.runBars(...bars(12), 'BTCUSDT');
  expectScenario(log, 'runBars');
  check(log[0] && log[0][0] === null && log[0][0] !== undefined,
        'flat reads null, not undefined or 0');
}

console.log('=== a second run on the same runner starts clean ===');
{
  const reg = new lrvx.SymbolRegistry();
  const sym = reg.addSymbol('sim', 'BTCUSDT', 0.01);
  const bt = new lrvx.BacktestRunner(reg, 0, 1e6);
  const log = [];
  const state = {n: 0};
  bt.setStrategy(scripted(sym, log, ACTIONS, state));
  // The first run ends long, two bars into the position.
  bt.runBars(...bars(4), 'BTCUSDT');
  check(log.length === 4 && near(log[3][0], 120) && log[3][1] === 2,
        `first run ends long at 120, 2 bars in (${JSON.stringify(log[3])})`);
  log.length = 0;
  state.n = 0;
  bt.runBars(...bars(12), 'BTCUSDT');
  expectScenario(log, 'second run');
}

console.log('=== fees do not change entryPrice ===');
{
  const reg = new lrvx.SymbolRegistry();
  const sym = reg.addSymbol('sim', 'BTCUSDT', 0.01);
  const bt = new lrvx.BacktestRunner(reg, 0.001, 1e6);
  const log = [];
  bt.setStrategy(scripted(sym, log, ACTIONS));
  bt.runBars(...bars(12), 'BTCUSDT');
  expectScenario(log, 'feeRate 0.001');
}

console.log('=== tick-only run: entryPrice is set, barsInPosition stays 0 ===');
{
  const reg = new lrvx.SymbolRegistry();
  const sym = reg.addSymbol('sim', 'BTCUSDT', 0.01);
  const bt = new lrvx.BacktestRunner(reg, 0, 1e6);
  const seen = [];
  const fills = [];
  let k = 0;
  bt.setStrategy({
    symbols: [sym],
    onTrade(ctx, _t, emit) {
      seen.push([ctx.entryPrice, ctx.barsInPosition, ctx.position]);
      if (k === 1) { emit.marketBuy(1); }
      if (k === 3) { emit.marketBuy(3); }
      k += 1;
    },
    onFill(_ctx, ev) { fills.push(ev.fillPrice); },
  });
  const ts = new BigInt64Array(7);
  for (let i = 0; i < 7; i++) { ts[i] = BigInt(i + 1) * 1000000000n; }
  bt.runOhlcv(ts, new Float64Array([100, 101, 103, 107, 111, 113, 117]), 'BTCUSDT');
  check(fills.length === 2, `two fills (${fills})`);
  check(seen[0] && seen[0][0] === null && seen[0][1] === 0, `flat before the buy (${JSON.stringify(seen[0])})`);
  if (fills.length === 2) {
    const avg = (fills[0] * 1 + fills[1] * 3) / 4;
    const afterFirst = seen.filter((s, i) => i >= 2 && i <= 3);
    afterFirst.forEach((s) => check(near(s[0], fills[0]),
                                    `after the first fill entryPrice ${s[0]} == fill ${fills[0]}`));
    const last = seen[seen.length - 1];
    check(near(last[0], avg), `after the add entryPrice ${last[0]} == weighted ${avg}`);
  }
  check(seen.every((s) => s[1] === 0), `barsInPosition 0 on every trade (${seen.map((s) => s[1])})`);
}

console.log('=== live Runner: fills via onOrderEvent, bars via onBar, per symbol ===');
{
  const reg = new lrvx.SymbolRegistry();
  const a = reg.addSymbol('test', 'AAA', 0.01);
  const b = reg.addSymbol('test', 'BBB', 0.01);
  const runner = new lrvx.Runner(reg, () => {}, false);
  const log = {[Number(a)]: [], [Number(b)]: []};
  let idA = 0;
  let idB = 0;
  let barsSeen = 0;
  runner.addStrategy({
    symbols: [a, b],
    onBar(ctx, _bar, emit) {
      log[ctx.symbolId].push([ctx.entryPrice, ctx.barsInPosition, ctx.position]);
      barsSeen += 1;
      if (!idA) { idA = emit.limitBuy(50, 2, a); }
      if (!idB) { idB = emit.limitSell(500, 1, b); }
    },
  });
  runner.start();
  const bar = (sym, i) => {
    const open = 100 + 10 * i;
    runner.onBar(sym, {open, high: open + 3, low: open - 2, close: open + 1, volume: 10,
                       startTimeNs: BigInt(i) * MIN, endTimeNs: BigInt(i + 1) * MIN});
  };
  bar(a, 0); bar(b, 0);
  runner.onOrderEvent({orderId: idA, symbol: a, side: 'buy', status: 'FILLED', fillQty: 1,
                       fillPrice: 50});
  bar(a, 1); bar(b, 1);
  runner.onOrderEvent({orderId: idA, symbol: a, side: 'buy', status: 'FILLED', fillQty: 3,
                       fillPrice: 70});
  runner.onOrderEvent({orderId: idB, symbol: b, side: 'sell', status: 'FILLED', fillQty: 1,
                       fillPrice: 500});
  bar(a, 2); bar(b, 2);
  bar(a, 3);
  runner.stop();
  const la = log[Number(a)];
  const lb = log[Number(b)];
  check(la.map((r) => r[2]).join() === '0,1,4,4' && lb.map((r) => r[2]).join() === '0,0,-1',
        `positions A ${la.map((r) => r[2])} B ${lb.map((r) => r[2])}`);
  check(barsSeen === 7, `seven bars reached the strategy (${barsSeen})`);
  check(la[0] && la[0][0] === null && la[0][1] === 0, `A flat on bar 0 (${JSON.stringify(la[0])})`);
  check(la[1] && near(la[1][0], 50) && la[1][1] === 1, `A long at 50, bar 1 (${JSON.stringify(la[1])})`);
  check(la[2] && near(la[2][0], 65) && la[2][1] === 2,
        `A add 3 @70: average 65, bar 2 (${JSON.stringify(la[2])})`);
  check(la[3] && near(la[3][0], 65) && la[3][1] === 3, `A bar 3 (${JSON.stringify(la[3])})`);
  check(lb[0] && lb[0][0] === null && lb[0][1] === 0, `B flat on bar 0 (${JSON.stringify(lb[0])})`);
  check(lb[1] && lb[1][0] === null && lb[1][1] === 0,
        `B still flat while A is long (${JSON.stringify(lb[1])})`);
  check(lb[2] && near(lb[2][0], 500) && lb[2][1] === 1,
        `B short at 500, its own first bar (${JSON.stringify(lb[2])})`);
}

// Scenario B: an add after a partial close, then a short added to, partly
// closed and added to again. Twenty bars, the same prices.
//   bar 1  buy 1  -> 120  long 1, entry 120
//   bar 3  buy 1  -> 140  long 2, entry 130
//   bar 5  sell 1 -> 160  long 1, entry 130 (the manager's FIFO basis: 140)
//   bar 7  buy 1  -> 180  long 2, entry (130 + 180) / 2 = 155 (FIFO: 160)
//   bar 9  sell 4 -> 200  short 2, entry 200, count restarts
//   bar 11 sell 2 -> 220  short 4, entry 210
//   bar 13 buy 1  -> 240  short 3, entry 210 (FIFO: 213.33)
//   bar 15 sell 1 -> 260  short 4, entry 222.5 (FIFO: 225)
//   bar 17 buy 4  -> 280  flat
console.log('=== add after a partial close, add to and partly close a short ===');
{
  const reg = new lrvx.SymbolRegistry();
  const sym = reg.addSymbol('sim', 'BTCUSDT', 0.01);
  const bt = new lrvx.BacktestRunner(reg, 0, 1e6);
  const actions = {1: ['buy', 1], 3: ['buy', 1], 5: ['sell', 1], 7: ['buy', 1], 9: ['sell', 4],
                   11: ['sell', 2], 13: ['buy', 1], 15: ['sell', 1], 17: ['buy', 4]};
  const log = [];
  const fills = [];
  const strat = scripted(sym, log, actions);
  strat.onFill = (ctx, ev) => {
    fills.push([ev.fillPrice, ctx.position, ctx.entryPrice, ctx.barsInPosition]);
  };
  bt.setStrategy(strat);
  bt.runBars(...bars(20), 'BTCUSDT');
  const want = [
    [null, 0], [null, 0], [120, 1], [120, 2], [130, 3], [130, 4], [130, 5], [130, 6],
    [155, 7], [155, 8], [200, 1], [200, 2], [210, 3], [210, 4], [210, 5], [210, 6],
    [222.5, 7], [222.5, 8], [null, 0], [null, 0],
  ];
  const positions = [0, 0, 1, 1, 2, 2, 1, 1, 2, 2, -2, -2, -4, -4, -3, -3, -4, -4, 0, 0];
  check(log.map((r) => r[2]).join() === positions.join(),
        `premise: positions ${log.map((r) => r[2])}`);
  want.forEach(([entry, held], i) => {
    const got = log[i] || [];
    const entryOk = entry === null ? got[0] === null : near(got[0], entry);
    check(entryOk, `scenario B bar ${i} entryPrice ${got[0]}, want ${entry}`);
    check(got[1] === held, `scenario B bar ${i} barsInPosition ${got[1]}, want ${held}`);
  });
  // An opening or flipping fill reads 0 bars; the flattening fill reads no
  // entry and 0 bars.
  const wantFills = [
    [120, 1, 120, 0], [140, 2, 130, 2], [160, 1, 130, 4], [180, 2, 155, 6], [200, -2, 200, 0],
    [220, -4, 210, 2], [240, -3, 210, 4], [260, -4, 222.5, 6], [280, 0, null, 0],
  ];
  check(fills.length === wantFills.length, `nine fills (${fills.length})`);
  wantFills.forEach(([price, pos, entry, held], i) => {
    const got = fills[i] || [];
    check(got[0] === price && got[1] === pos, `premise: fill ${i} ${got[1]} @ ${got[0]}`);
    const entryOk = entry === null ? got[2] === null : near(got[2], entry);
    check(entryOk, `onFill ${i}: entryPrice ${got[2]}, want ${entry}`);
    check(got[3] === held, `onFill ${i}: barsInPosition ${got[3]}, want ${held}`);
  });
}

// A threaded Runner calls the strategy later, on the JS thread, than the
// engine thread delivered the event; ctx carries the facts as they were at
// delivery. Ordering: market data published after a reported fill may reach
// the strategy before it, so each fill is awaited (its onFill has run)
// before the next bar is published. To have the facts move between delivery
// and the callback, two bars are published back to back and the JS thread
// is held until the engine has handed over the second (it shows up as the
// last closed bar): the first bar's callback then runs after the count has
// already moved on.
async function threadedRunnerSection() {
  console.log('=== threaded Runner: ctx facts are those at delivery ===');
  const DEADLINE_MS = 5000;
  const waitFor = async (cond, what) => {
    const until = Date.now() + DEADLINE_MS;
    while (!cond()) {
      if (Date.now() > until) { throw new Error(`timed out after ${DEADLINE_MS} ms waiting for ${what}`); }
      await new Promise((r) => setImmediate(r));
    }
  };
  // Blocks the JS thread, so no callback can run, until cond() holds.
  const holdUntil = (cond, what) => {
    const until = Date.now() + DEADLINE_MS;
    const cell = new Int32Array(new SharedArrayBuffer(4));
    while (!cond()) {
      if (Date.now() > until) { throw new Error(`timed out after ${DEADLINE_MS} ms waiting for ${what}`); }
      Atomics.wait(cell, 0, 0, 1);
    }
  };
  const reg = new lrvx.SymbolRegistry();
  const a = reg.addSymbol('test', 'AAA', 0.01);
  const runner = new lrvx.Runner(reg, () => {}, true);
  const log = [];
  const fills = [];
  let emitter = null;
  let idBuy = 0;
  let idSell = 0;
  runner.addStrategy({
    symbols: [a],
    onBar(ctx, _bar, emit) {
      log.push([ctx.entryPrice, ctx.barsInPosition, ctx.position]);
      if (!idBuy) {
        emitter = emit;
        idBuy = emit.limitBuy(50, 4, a);
        idSell = emit.limitSell(90, 4, a);
      }
    },
    onFill(ctx, ev) {
      fills.push([ev.fillPrice, ctx.entryPrice, ctx.barsInPosition, ctx.position]);
    },
  });
  runner.start();
  const bar = (i) => {
    const open = 100 + 10 * i;
    runner.onBar(a, {open, high: open + 3, low: open - 2, close: open + 1, volume: 10,
                     startTimeNs: BigInt(i) * MIN, endTimeNs: BigInt(i + 1) * MIN});
  };
  const delivered = (i) => () => {
    const last = emitter.lastClosedBar(a, 0, 0);
    return last !== null && last.startNs === BigInt(i) * MIN;
  };
  const fill = async (orderId, side, fillQty, fillPrice) => {
    const n = fills.length;
    runner.onOrderEvent({orderId, symbol: a, side, status: 'FILLED', fillQty, fillPrice});
    await waitFor(() => fills.length === n + 1, `the fill of ${fillQty} @ ${fillPrice}`);
  };
  try {
    bar(0);
    await waitFor(() => log.length === 1, 'bar 0');
    await fill(idBuy, 'buy', 1, 50);
    bar(1);   // long 1 @ 50, bar 1
    bar(2);   // long 1 @ 50, bar 2
    holdUntil(delivered(2), 'the engine to deliver bar 2');
    await waitFor(() => log.length === 3, 'bars 1 and 2');
    await fill(idBuy, 'buy', 3, 70);
    bar(3);   // long 4 @ 65, bar 3
    await waitFor(() => log.length === 4, 'bar 3');
    await fill(idSell, 'sell', 4, 80);
    bar(4);   // flat
    await waitFor(() => log.length === 5, 'bar 4');
  } finally {
    runner.stop();
  }
  const show = (r) => JSON.stringify(r);
  check(log[0] && log[0][0] === null && log[0][1] === 0, `bar 0 flat (${show(log[0])})`);
  check(log[1] && log[1][2] === 1 && near(log[1][0], 50) && log[1][1] === 1,
        `bar 1 long 1 @ 50, 1 bar (${show(log[1])})`);
  check(log[2] && log[2][2] === 1 && near(log[2][0], 50) && log[2][1] === 2,
        `bar 2 long 1 @ 50, 2 bars (${show(log[2])})`);
  check(log[3] && log[3][2] === 4 && near(log[3][0], 65) && log[3][1] === 3,
        `bar 3 long 4 @ 65, 3 bars (${show(log[3])})`);
  check(log[4] && log[4][2] === 0 && log[4][0] === null && log[4][1] === 0,
        `bar 4 flat (${show(log[4])})`);
  check(fills[0] && near(fills[0][1], 50) && fills[0][2] === 0, `fill 0 (${show(fills[0])})`);
  check(fills[1] && near(fills[1][1], 65) && fills[1][2] === 2, `fill 1 (${show(fills[1])})`);
  check(fills[2] && fills[2][3] === 0 && fills[2][1] === null && fills[2][2] === 0,
        `the flattening fill reads no entry and 0 bars (${show(fills[2])})`);
}

threadedRunnerSection()
  .catch((e) => check(false, `threaded Runner section threw ${e && e.stack}`))
  .then(() => {
    console.log(`\n${passed} passed, ${failed} failed`);
    process.exit(failed > 0 ? 1 : 0);
  });
