'use strict';
/**
 * node/test/test_latency.js — smoke-test latency model NAPI bindings.
 *
 * Run from repo root:
 *   cd node && node test/test_latency.js
 */

const path = require('path');
const lrvx = require(path.join(__dirname, '..'));

let passed = 0;
let failed = 0;

function check(condition, msg) {
  if (condition) {
    console.log(`  ok  ${msg}`);
    passed++;
  } else {
    console.error(`  FAIL  ${msg}`);
    failed++;
  }
}

// ── ConstantLatency ────────────────────────────────────────────────

console.log('=== ConstantLatency ===');
{
  const c = new lrvx.ConstantLatency({ feedNs: 100, orderNs: 200, fillNs: 300 });
  check(c.feedDelay() === 100, 'feed_delay returns configured feed_ns');
  check(c.orderDelay() === 200, 'order_delay returns configured order_ns');
  check(c.fillDelay() === 300, 'fill_delay returns configured fill_ns');
  const s = c.sample();
  check(s.feedNs === 100 && s.orderNs === 200 && s.fillNs === 300,
        'sample returns LatencySample with all three');
  // Two calls are byte-identical for constant.
  check(c.feedDelay() === c.feedDelay(), 'constant is deterministic');
}
{
  let threw = false;
  try { new lrvx.ConstantLatency({ feedNs: -1 }); }
  catch (e) { threw = true; check(e.code === 'E_VAL_002', 'negative feedNs throws E_VAL_002'); }
  check(threw, 'negative feedNs rejected');
}

// ── GaussianLatency ────────────────────────────────────────────────

console.log('=== GaussianLatency ===');
{
  const g = new lrvx.GaussianLatency({
    feedMeanNs: 1000, feedStddevNs: 200,
    orderMeanNs: 500, orderStddevNs: 50,
    fillMeanNs: 2000, fillStddevNs: 300,
    seed: 42,
  });
  // Reproducibility under the same seed.
  const a = [g.feedDelay(), g.orderDelay(), g.fillDelay()];
  g.reset(42);
  const b = [g.feedDelay(), g.orderDelay(), g.fillDelay()];
  check(a[0] === b[0] && a[1] === b[1] && a[2] === b[2],
        'gaussian sequence is reproducible under reset(seed)');
  // Non-negativity.
  let allNonNeg = true;
  for (let i = 0; i < 50; i++) {
    if (g.feedDelay() < 0 || g.orderDelay() < 0 || g.fillDelay() < 0) {
      allNonNeg = false; break;
    }
  }
  check(allNonNeg, 'gaussian samples are clamped non-negative');
  // Stddev=0 collapses to mean.
  const collapsed = new lrvx.GaussianLatency({ feedMeanNs: 1500, feedStddevNs: 0, seed: 0 });
  check(collapsed.feedDelay() === 1500, 'stddev=0 collapses to mean');
}

// ── ExponentialLatency ─────────────────────────────────────────────

console.log('=== ExponentialLatency ===');
{
  const e = new lrvx.ExponentialLatency({ feedMeanNs: 500, seed: 7 });
  let nonNegSeen = true;
  for (let i = 0; i < 50; i++) {
    if (e.feedDelay() < 0) { nonNegSeen = false; break; }
  }
  check(nonNegSeen, 'exponential samples are non-negative');
  const zero = new lrvx.ExponentialLatency({ feedMeanNs: 0, seed: 0 });
  check(zero.feedDelay() === 0, 'zero mean -> zero delay');
}

// ── EmpiricalLatency ───────────────────────────────────────────────

console.log('=== EmpiricalLatency ===');
{
  const emp = new lrvx.EmpiricalLatency({ feedSamples: [10, 20, 30], seed: 0 });
  const allowed = new Set([10, 20, 30]);
  let allInSet = true;
  for (let i = 0; i < 30; i++) {
    if (!allowed.has(emp.feedDelay())) { allInSet = false; break; }
  }
  check(allInSet, 'empirical draws are subset of input samples');
  check(emp.orderDelay() === 0, 'empirical with no order_samples returns 0');
}
{
  let threw = false;
  try { new lrvx.EmpiricalLatency({}); }
  catch (e) { threw = true; check(e.code === 'E_VAL_002', 'empty empirical throws E_VAL_002'); }
  check(threw, 'all-empty empirical rejected');
}

// ── SimulatedExecutor: cancel-all and the cancel ack latency ───────

console.log('=== cancelAll honours the cancel ack latency ===');
{
  // A cross inside the ack window still fills the order cancelAll is
  // pulling, exactly as it would under cancelOrder.
  const cancelAllThenTrade = (cancelAckNs) => {
    const ex = new lrvx.SimulatedExecutor();
    if (cancelAckNs > 0) ex.setCancelAckLatency(cancelAckNs, 0);
    ex.advanceClock(1_000_000_000);
    ex.onTopOfBook(1, { price: 100, qty: 1 }, { price: 101, qty: 5 });
    ex.submitOrder(1, 'buy', 100, 1, 'limit', 1);
    ex.cancelAll(1);
    ex.advanceClock(1_001_000_000);
    // The ask drops through the resting buy inside the ack window.
    ex.onTopOfBook(1, { price: 99, qty: 1 }, { price: 99.5, qty: 5 });
    return ex.fillCount;
  };
  check(cancelAllThenTrade(0) === 0, 'without an ack latency cancelAll cancels at once');
  check(cancelAllThenTrade(10_000_000) === 1, 'with an ack latency the order stays live until the ack');
}

console.log('=== setDefaultSlippage refuses an unknown model ===');
{
  const ex = new lrvx.SimulatedExecutor();
  let msg = '';
  try { ex.setDefaultSlippage('fixed_tick', 1, 0.01, 0, 0); } catch (e) { msg = String(e.message); }
  check(msg.includes("'fixed_ticks'") && msg.includes("'volume_impact'") && msg.includes("'fixed_tick'"),
        `unknown name throws with the valid names: ${msg}`);
  msg = '';
  try { ex.setDefaultSlippage(9, 0, 0, 0, 0); } catch (e) { msg = String(e.message); }
  check(msg.includes('3 (volume_impact)'), `out-of-range code throws: ${msg}`);
  let ok = true;
  try {
    ex.setDefaultSlippage('fixed_bps', 0, 0, 5, 0);
    ex.setDefaultSlippage(lrvx.SLIPPAGE_NONE, 0, 0, 0, 0);
  } catch (e) { ok = false; }
  check(ok, 'valid names and SLIPPAGE_* codes are accepted');
}

if (failed > 0) {
  console.error(`\n${failed} check(s) failed`);
  process.exit(1);
}
console.log(`\n${passed} check(s) passed`);
