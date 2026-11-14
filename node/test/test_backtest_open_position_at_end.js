'use strict';
/**
 * node/test/test_backtest_open_position_at_end.js -- the position still open
 * when a backtest ends, valued.
 *
 * netPnl, totalPnl, totalFees, trades() and equityCurve() keep their meaning
 * (closed trades; every fill's fee). The stats also carry:
 *   unrealizedPnl  sum over symbols of (mark - entry) * qty for a long,
 *                  (entry - mark) * qty for a short, no exit fee
 *   finalEquity    initialCapital + netPnl + unrealizedPnl
 * where the mark is the last price the run saw for the symbol: a bar's
 * close on bar replay, the last trade on tick replay.
 *
 * BacktestRunner.openPositions() and BacktestResult.openPositions() return
 * arrays aligned by row, like trades(): symbol (Uint32Array), side
 * (Uint8Array, 0 long / 1 short), quantity, entryPrice, markPrice,
 * unrealizedPnl (Float64Array), ascending symbol id. BacktestResult takes the
 * mark by hand with markPrice(symbol, price); an unmarked symbol is valued at
 * its last fill.
 *
 * Run from repo root:
 *   cd node && node test/test_backtest_open_position_at_end.js
 */

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

const FEE = 0.0004;
const CAPITAL = 10000;
const SEC = 1_000_000_000n;
const MIN = 60n * SEC;
const T0 = 1767225600n * SEC;

function equityIdentity(s, label) {
  check(near(s.finalEquity, s.initialCapital + s.netPnl + s.unrealizedPnl),
        `${label}: finalEquity = initialCapital + netPnl + unrealizedPnl (${s.finalEquity})`);
}

function runner() {
  const reg = new lrvx.SymbolRegistry();
  const sym = reg.addSymbol('test', 'BTCUSDT', 0.01);
  return { bt: new lrvx.BacktestRunner(reg, FEE, CAPITAL), sym };
}

// ── runOhlcv: tick replay, marked at the last close ──────────────────
for (const qty of [0.5, -0.5]) {
  attempt(`runOhlcv ${qty}`, () => {
    const { bt, sym } = runner();
    let n = 0;
    bt.setStrategy({
      symbols: [sym],
      onTrade(_ctx, _trade, emit) {
        if (n++ === 0) { qty > 0 ? emit.marketBuy(qty) : emit.marketSell(-qty); }
      },
    });
    const closes = [100, 100, 100, 104, 107];
    const ts = BigInt64Array.from(closes.map((_, i) => T0 + BigInt(i) * MIN));
    const s = bt.runOhlcv(ts, Float64Array.from(closes), 'BTCUSDT');
    const label = `runOhlcv ${qty > 0 ? 'long' : 'short'}`;
    const expected = qty > 0 ? 3.5 : -3.5;
    check(near(s.unrealizedPnl, expected), `${label}: unrealizedPnl ${s.unrealizedPnl} == ${expected}`);
    equityIdentity(s, label);
    const open = bt.openPositions();
    check(open.symbol.length === 1 && open.symbol[0] === Number(sym), `${label}: one open position`);
    check(open.side[0] === (qty > 0 ? 0 : 1), `${label}: side`);
    check(near(open.quantity[0], 0.5), `${label}: quantity`);
    check(near(open.entryPrice[0], 100), `${label}: entryPrice`);
    check(near(open.markPrice[0], 107), `${label}: markPrice is the last close`);
    check(near(open.unrealizedPnl[0], expected), `${label}: row unrealizedPnl`);
  });
}

attempt('runOhlcv flat', () => {
  const { bt, sym } = runner();
  let n = 0;
  bt.setStrategy({
    symbols: [sym],
    onTrade(_ctx, _trade, emit) {
      if (n === 0) { emit.marketBuy(1); } else if (n === 2) { emit.marketSell(1); }
      n++;
    },
  });
  const closes = [100, 101, 102, 103, 150];
  const ts = BigInt64Array.from(closes.map((_, i) => T0 + BigInt(i) * MIN));
  const s = bt.runOhlcv(ts, Float64Array.from(closes), 'BTCUSDT');
  check(s.totalTrades === 1, 'flat: one closed trade');
  check(s.unrealizedPnl === 0, `flat: unrealizedPnl is 0 (${s.unrealizedPnl})`);
  check(near(s.finalEquity, s.finalCapital), `flat: finalEquity == finalCapital (${s.finalEquity})`);
  check(bt.openPositions().symbol.length === 0, 'flat: no open position');
});

// ── runBars: marked at the last bar close ────────────────────────────
for (const qty of [2, -2]) {
  attempt(`runBars ${qty}`, () => {
    const { bt, sym } = runner();
    let n = 0;
    bt.setStrategy({
      symbols: [sym],
      onBar(_ctx, _bar, emit) {
        if (n++ === 0) { qty > 0 ? emit.marketBuy(qty) : emit.marketSell(-qty); }
      },
    });
    const start = BigInt64Array.from([T0, T0 + MIN, T0 + 2n * MIN]);
    const end = BigInt64Array.from([T0 + MIN, T0 + 2n * MIN, T0 + 3n * MIN]);
    const s = bt.runBars(start, end, Float64Array.from([100, 101, 103]),
                         Float64Array.from([100.5, 103, 108]), Float64Array.from([99.5, 100, 96]),
                         Float64Array.from([100, 102, 97]), Float64Array.from([10, 10, 10]),
                         'BTCUSDT');
    const label = `runBars ${qty > 0 ? 'long' : 'short'}`;
    // Filled at bar 1's open (101), marked at bar 2's close (97).
    const expected = qty > 0 ? -8 : 8;
    check(near(s.unrealizedPnl, expected), `${label}: unrealizedPnl ${s.unrealizedPnl} == ${expected}`);
    equityIdentity(s, label);
    const open = bt.openPositions();
    check(near(open.entryPrice[0], 101), `${label}: entryPrice`);
    check(near(open.markPrice[0], 97), `${label}: markPrice`);
  });
}

// ── BacktestResult by hand: two symbols summed ───────────────────────
attempt('BacktestResult', () => {
  const r = new lrvx.BacktestResult(CAPITAL, FEE);
  r.recordFill(1, 1, 'buy', 100, 1, T0);
  r.recordFill(2, 1, 'sell', 110, 1, T0 + SEC);
  r.recordFill(3, 1, 'buy', 105, 2, T0 + 2n * SEC);   // long 2 @ 105
  r.recordFill(4, 2, 'sell', 50, 3, T0 + 3n * SEC);   // short 3 @ 50
  r.markPrice(1, 108.5);                               // +7
  r.markPrice(2, 52);                                  // -6
  const s = r.stats();
  check(near(s.unrealizedPnl, 1), `BacktestResult: unrealizedPnl sums the symbols (${s.unrealizedPnl})`);
  equityIdentity(s, 'BacktestResult');
  check(near(s.totalPnl, 10), 'BacktestResult: totalPnl is the closed trade only');
  const open = r.openPositions();
  check(Array.from(open.symbol).join() === '1,2', 'BacktestResult: ascending symbol id');
  check(Array.from(open.side).join() === '0,1', 'BacktestResult: sides');
  check(near(open.quantity[0], 2) && near(open.quantity[1], 3), 'BacktestResult: quantities');
  check(near(open.entryPrice[0], 105) && near(open.entryPrice[1], 50), 'BacktestResult: entries');
  check(near(open.markPrice[0], 108.5) && near(open.markPrice[1], 52), 'BacktestResult: marks');
  check(near(open.unrealizedPnl[0], 7) && near(open.unrealizedPnl[1], -6), 'BacktestResult: rows');

  const u = new lrvx.BacktestResult(CAPITAL, 0);
  u.recordFill(1, 1, 'buy', 100, 2, T0);
  u.recordFill(2, 1, 'sell', 104, 1, T0 + SEC);
  check(near(u.stats().unrealizedPnl, 4), 'BacktestResult: an unmarked symbol is valued at its last fill');
});

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed === 0 ? 0 : 1);
