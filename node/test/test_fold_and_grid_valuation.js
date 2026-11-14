'use strict';
/**
 * node/test/test_fold_and_grid_valuation.js -- the position still open at
 * the end of a walk-forward window and of a grid-search combination, valued.
 *
 * A WalkForwardRunner fold's trainStats and testStats carry unrealizedPnl
 * (the position open at the end of that window, marked at its last price:
 * the last close of the window) and finalEquity (initialCapital + netPnl +
 * unrealizedPnl), as Python's fold dicts and the BacktestRunner stats do.
 *
 * A GridSearch result's stats carry the unrealizedPnl and finalEquity its
 * factory returned, so a BacktestRunner stats object returned as is keeps
 * them. A factory that leaves them out reads 0, like every other field.
 *
 * Run from repo root:
 *   cd node && node test/test_fold_and_grid_valuation.js
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
function near(a, b, eps = 1e-9) { return typeof a === 'number' && Math.abs(a - b) <= eps; }
function attempt(label, fn) {
  try { fn(); } catch (e) { failed++; console.error(`  FAIL  ${label}: threw ${e.message}`); }
}

const CAPITAL = 10000;
const SEC = 1_000_000_000n;
const T0 = 1767225600n * SEC;

// Three windows of four bars, each flat for three bars and then up: a
// position bought on a window's first trade is up 10, 30 and 50 at the
// window's last close.
const CLOSES = [100, 100, 100, 110, 200, 200, 200, 230, 300, 300, 300, 350];
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'lrvx-fold-valuation-'));
const csv = path.join(dir, 'ramp.csv');
fs.writeFileSync(csv, 'timestamp,open,high,low,close,volume\n' +
  CLOSES.map((c, i) => `${T0 + BigInt(i) * 60n * SEC},${c},${c},${c},${c},1`).join('\n') + '\n');

function windowStrategy(sym, closeAt) {
  let n = 0;
  return {
    symbols: [sym],
    onTrade(_ctx, _trade, emit) {
      if (n === 0) { emit.marketBuy(1); } else if (closeAt > 0 && n === closeAt) { emit.marketSell(1); }
      n++;
    },
  };
}

function walkForward(feeRate, closeAt) {
  const reg = new lrvx.SymbolRegistry();
  const sym = reg.addSymbol('test', 'BTCUSDT', 0.01);
  const wf = new lrvx.WalkForwardRunner(reg, feeRate, CAPITAL,
                                        { mode: 'sliding', trainSize: 4, testSize: 4, step: 4 });
  wf.setStrategyFactory(() => windowStrategy(sym, closeAt));
  return wf.runCsv(csv, 'BTCUSDT');
}

function identity(s, label) {
  check(near(s.finalEquity, s.initialCapital + s.netPnl + s.unrealizedPnl),
        `${label}: finalEquity = initialCapital + netPnl + unrealizedPnl (${s.finalEquity})`);
}

// ── WalkForwardRunner: each leg marked at its window's last close ──────
attempt('walk-forward open', () => {
  const folds = walkForward(0, 0);
  check(folds.length === 2, `two folds (${folds.length})`);
  const expected = [[10, 30], [30, 50]];
  folds.forEach((f, i) => {
    const [train, test] = expected[i];
    check(near(f.trainStats.unrealizedPnl, train),
          `fold ${i} train: unrealizedPnl ${f.trainStats.unrealizedPnl} == ${train}`);
    check(near(f.testStats.unrealizedPnl, test),
          `fold ${i} test: unrealizedPnl ${f.testStats.unrealizedPnl} == ${test}`);
    check(near(f.trainStats.finalEquity, CAPITAL + train),
          `fold ${i} train: finalEquity ${f.trainStats.finalEquity} == ${CAPITAL + train}`);
    check(near(f.testStats.finalEquity, CAPITAL + test),
          `fold ${i} test: finalEquity ${f.testStats.finalEquity} == ${CAPITAL + test}`);
    identity(f.trainStats, `fold ${i} train`);
    identity(f.testStats, `fold ${i} test`);
    check(f.testStats.finalCapital === CAPITAL, `fold ${i} test: finalCapital leaves the open unit out`);
  });
});

attempt('walk-forward with fees', () => {
  const folds = walkForward(0.0004, 0);
  folds.forEach((f, i) => {
    identity(f.trainStats, `fees fold ${i} train`);
    identity(f.testStats, `fees fold ${i} test`);
  });
  check(folds.length === 2 && near(folds[1].testStats.unrealizedPnl, 50),
        'fees: no exit fee comes off the open unit');
});

attempt('walk-forward flat', () => {
  const folds = walkForward(0, 2);
  check(folds.length === 2, 'flat: two folds');
  folds.forEach((f, i) => {
    for (const [leg, s] of [['train', f.trainStats], ['test', f.testStats]]) {
      check(s.totalTrades === 1, `flat fold ${i} ${leg}: one closed trade`);
      check(s.unrealizedPnl === 0, `flat fold ${i} ${leg}: unrealizedPnl is 0 (${s.unrealizedPnl})`);
      check(near(s.finalEquity, s.finalCapital),
            `flat fold ${i} ${leg}: finalEquity == finalCapital (${s.finalEquity})`);
    }
  });
});

// ── GridSearch: the factory's valuation reads through ─────────────────
attempt('grid search by hand', () => {
  const gs = new lrvx.GridSearch();
  gs.addAxis([1, 2]);
  gs.addAxis([-3, 7]);
  gs.setFactory((p) => ({
    initialCapital: CAPITAL,
    netPnl: 10 * p[0],
    unrealizedPnl: p[1],
    finalEquity: CAPITAL + 10 * p[0] + p[1],
  }));
  const rows = gs.run();
  check(rows.length === 4, 'four combinations');
  rows.forEach((r) => {
    const [a, b] = r.params;
    check(near(r.stats.unrealizedPnl, b), `[${a},${b}]: unrealizedPnl ${r.stats.unrealizedPnl} == ${b}`);
    check(near(r.stats.finalEquity, CAPITAL + 10 * a + b),
          `[${a},${b}]: finalEquity ${r.stats.finalEquity} == ${CAPITAL + 10 * a + b}`);
  });
});

attempt('grid search over a runner', () => {
  // Each combination runs a BacktestRunner that buys on the first trade and
  // holds; its stats are returned as is.
  const gs = new lrvx.GridSearch();
  gs.addAxis([0.5, 2]);
  const direct = [];
  gs.setFactory((p) => {
    const reg = new lrvx.SymbolRegistry();
    const sym = reg.addSymbol('test', 'BTCUSDT', 0.01);
    const bt = new lrvx.BacktestRunner(reg, 0.0004, CAPITAL);
    let n = 0;
    bt.setStrategy({ symbols: [sym], onTrade(_c, _t, emit) { if (n++ === 0) emit.marketBuy(p[0]); } });
    const closes = [100, 100, 100, 104, 107];
    const ts = BigInt64Array.from(closes.map((_, i) => T0 + BigInt(i) * 60n * SEC));
    const s = bt.runOhlcv(ts, Float64Array.from(closes), 'BTCUSDT');
    direct.push(s);
    return s;
  });
  const rows = gs.run();
  check(rows.length === 2 && direct.length === 2, 'two combinations, two runs');
  rows.forEach((r, i) => {
    const want = 7 * r.params[0];
    check(near(direct[i].unrealizedPnl, want), `runner ${i}: its own unrealizedPnl is ${want}`);
    check(near(r.stats.unrealizedPnl, direct[i].unrealizedPnl),
          `runner ${i}: grid unrealizedPnl ${r.stats.unrealizedPnl} == ${direct[i].unrealizedPnl}`);
    check(near(r.stats.finalEquity, direct[i].finalEquity),
          `runner ${i}: grid finalEquity ${r.stats.finalEquity} == ${direct[i].finalEquity}`);
  });
});

attempt('grid search left out', () => {
  const gs = new lrvx.GridSearch();
  gs.addAxis([1, 2]);
  gs.setFactory((p) => ({ returnPct: p[0] }));
  const rows = gs.run();
  check(rows[0].stats.unrealizedPnl === 0 && rows[1].stats.unrealizedPnl === 0,
        `left out: unrealizedPnl reads 0 (${rows[0].stats.unrealizedPnl})`);
  check(rows[0].stats.finalEquity === 0 && rows[1].stats.finalEquity === 0,
        `left out: finalEquity reads 0 (${rows[0].stats.finalEquity})`);
});

attempt('grid search wrong type', () => {
  const gs = new lrvx.GridSearch();
  gs.addAxis([1]);
  gs.setFactory(() => ({ unrealizedPnl: '3' }));
  try {
    gs.run();
    check(false, 'a string unrealizedPnl is refused');
  } catch (e) {
    check(e instanceof TypeError && /unrealizedPnl/.test(e.message),
          `a string unrealizedPnl is refused (${e.constructor.name}: ${e.message})`);
  }
});

fs.rmSync(dir, { recursive: true, force: true });
console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed === 0 ? 0 : 1);
