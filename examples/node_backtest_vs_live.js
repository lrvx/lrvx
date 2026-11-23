/**
 * Backtest and live with the same strategy class — Node.js.
 *
 * The same strategy object works in three modes:
 *   - BacktestRunner:       replay CSV, SimulatedExecutor fills orders
 *   - Runner:               synchronous live, push ticks from your connector
 *   - Runner(reg, cb, true): Disruptor-based, lock-free publish
 *
 * Usage:
 *   node examples/node_backtest_vs_live.js
 */

const path = require('path');
const lrvx = require('../../node');

const DATA = path.join(__dirname, 'data', 'btcusdt_1m.csv');

// ── Symbol registry (shared across all modes) ──────────────────────────

const registry = new lrvx.SymbolRegistry();
const btc = registry.addSymbol('binance', 'BTCUSDT', 0.01);

// ── Strategy factory — plain JS object with callbacks ─────────────────
//
// A new instance is created for each mode so state is fresh.
// In backtest mode the emit object is wired to SimulatedExecutor.
// In live modes it routes orders to the exchange.

function makeSMAStrategy() {
  const fast = new lrvx.SMA(10);
  const slow  = new lrvx.SMA(30);
  let tradeCount = 0;

  return {
    symbols: [btc],

    onStart() {
      tradeCount = 0;
    },

    onStop() {
      console.log(`  SMAStrategy stopped  (${tradeCount} signals emitted)`);
    },

    onTrade(ctx, trade, emit) {
      const fv = fast.update(trade.price);
      const sv = slow.update(trade.price);
      if (!slow.ready) return;

      if (fv > sv && ctx.position === 0) {
        emit.marketBuy(0.01);
        tradeCount++;
      } else if (fv < sv && ctx.position === 0) {
        emit.marketSell(0.01);
        tradeCount++;
      }
    },
  };
}

// ── 1. BacktestRunner — replay historical CSV ─────────────────────────

console.log('── Backtest ──────────────────────────────────────────────────────');

const bt = new lrvx.BacktestRunner(registry, 0.0004, 10_000);
bt.setStrategy(makeSMAStrategy());
const stats = bt.runCsv(DATA, 'BTCUSDT');

if (stats) {
  console.log(`  Return   : ${stats.returnPct >= 0 ? '+' : ''}${stats.returnPct.toFixed(4)}%`);
  console.log(`  Trades   : ${stats.totalTrades}  win=${(stats.winRate * 100).toFixed(1)}%`);
  console.log(`  Sharpe   : ${stats.sharpeRatio.toFixed(4)}`);
  console.log(`  Max DD   : ${stats.maxDrawdownPct.toFixed(4)}%`);
  console.log(`  Net PnL  : ${stats.netPnl.toFixed(4)}`);
} else {
  console.log('  backtest failed (CSV not found?)');
}

// ── 2. Runner — synchronous live ──────────────────────────────────────

console.log('\n── Runner (live, sync) ───────────────────────────────────────────');

const liveSigs = [];
const runner = new lrvx.Runner(registry, sig => liveSigs.push(sig));
runner.addStrategy(makeSMAStrategy());
runner.start();

const prices = Array.from({ length: 40 }, (_, i) => 50000 + i * 50);
let tsNs = BigInt(Date.now()) * 1_000_000n;
for (let i = 0; i < prices.length; i++) {
  runner.onTrade(btc, prices[i], 0.1, i % 2 === 0, Number(tsNs));
  tsNs += 1_000_000_000n;
}
runner.stop();

console.log(`  Signals received: ${liveSigs.length}`);
for (const s of liveSigs.slice(0, 3)) {
  console.log(`    ${s.side.padEnd(4)}  ${s.quantity.toFixed(4)} @ ${s.price.toFixed(2)}  [${s.orderType}]`);
}

// ── 3. Runner(threaded=true) — Disruptor, lock-free publish ───────────

console.log('\n── Runner (threaded=true) ────────────────────────────────────────');

const engineSigs = [];
const threaded = new lrvx.Runner(registry, sig => engineSigs.push(sig), true);
threaded.addStrategy(makeSMAStrategy());
threaded.start();

tsNs = BigInt(Date.now()) * 1_000_000n;
for (let i = 0; i < prices.length; i++) {
  threaded.onTrade(btc, prices[i], 0.1, i % 2 === 0, Number(tsNs));
  tsNs += 1_000_000_000n;
}

setTimeout(() => {
  threaded.stop();
  console.log(`  Signals received: ${engineSigs.length}`);
  process.exit(0);
}, 50);
