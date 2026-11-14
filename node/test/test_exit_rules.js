'use strict';
/**
 * node/test/test_exit_rules.js -- the exit rules for bar strategies.
 *
 * AtrStop, ChandelierExit and BreakEvenTrail pin the scenario of
 * tests/test_exits.cpp and python/tests/test_exits.py to the same levels;
 * PercentStop, TakeProfit and TimeStop pin the scenarios their C++ and Python
 * tests pin. One shape: arm(side, entryPrice[, atr]), update(high, low[, atr])
 * -> true when the bar reached the level the previous update left, stop /
 * target / armed read-only properties, disarm(). A hit does not disarm.
 *
 * Run from repo root:
 *   cd node && node test/test_exit_rules.js
 */

const path = require('path');
const lrvx = require(path.join(__dirname, '..'));

let passed = 0;
let failed = 0;
function check(cond, msg) {
  if (cond) { passed++; console.log(`  ok  ${msg}`); }
  else { failed++; console.error(`  FAIL  ${msg}`); }
}
function throws(fn, type, msg) {
  try {
    fn();
    check(false, `${msg} (did not throw)`);
  } catch (e) {
    check(e instanceof type, `${msg} (${e && e.name}: ${e && e.message})`);
  }
}
// Each section runs on its own, so a missing class fails its checks and the
// file goes on to the next.
function section(name, body) {
  console.log(`=== ${name} ===`);
  try {
    body();
  } catch (e) {
    check(false, `${name}: threw ${e && e.name}: ${e && e.message}`);
  }
}

const LONG_BARS = [
  [103.0, 99.0, 2.0],
  [106.0, 102.0, 2.0],
  [110.0, 105.0, 2.5],
  [108.0, 104.0, 2.5],
  [105.0, 101.5, 2.5],
  [103.0, 97.0, 3.0],
];

section('AtrStop is frozen at entry minus a multiple of ATR', () => {
  const s = new lrvx.AtrStop(2.0);
  check(s.armed === false, 'not armed before arm');
  check(s.update(101.0, 99.0, 2.0) === false, 'nothing is watched before arm');
  s.arm('long', 100.0, 2.0);
  check(s.armed === true, 'armed');
  check(s.stop === 96.0, `stop 96 (${s.stop})`);
  let hit = false;
  for (const [h, l, a] of LONG_BARS) {
    hit = s.update(h, l, a);
    check(s.stop === 96.0, `an ATR stop does not move (${s.stop})`);
    if (hit) { break; }
  }
  check(hit === false, 'the lowest low, 97, stays above 96');
  s.arm('short', 100.0, 2.0);
  check(s.stop === 104.0, `short stop 104 (${s.stop})`);
  check(s.update(103.0, 99.0, 2.0) === false, 'high 103 below 104');
  check(s.update(104.0, 101.0, 2.0) === true, 'a short is stopped when high >= stop');
  check(s.update(104.0, 101.0, 2.0) === true, 'a hit is reported until disarm');
  s.disarm();
  check(s.armed === false && Number.isNaN(s.stop), `disarm: not armed, stop NaN (${s.stop})`);
  throws(() => s.arm('buy', 100.0, 2.0), RangeError, "side 'buy' is rejected");
});

section('ChandelierExit hangs below the highest high and only rises', () => {
  const r = new lrvx.ChandelierExit(0, 3.0);
  r.arm('long', 100.0, 2.0);
  check(r.stop === 94.0, `entry - 3 * 2 = 94 (${r.stop})`);
  const want = [97.0, 100.0, 102.5, 102.5];
  LONG_BARS.slice(0, 4).forEach(([h, l, a], i) => {
    check(r.update(h, l, a) === false, `bar ${i} not hit`);
    check(r.stop === want[i], `bar ${i} stop ${r.stop}, want ${want[i]}`);
  });
  check(r.update(105.0, 101.5, 2.5) === true, 'low 101.5 reaches 102.5');
  check(r.stop === 102.5, `stop holds at 102.5 (${r.stop})`);
});

section('ChandelierExit lookback drops old highs but the stop never falls', () => {
  const r = new lrvx.ChandelierExit(2, 3.0);
  r.arm('long', 100.0, 2.0);
  for (const [h, l, a] of LONG_BARS.slice(0, 4)) { check(r.update(h, l, a) === false, 'no hit'); }
  check(r.stop === 102.5, `102.5 (${r.stop})`);
  check(r.update(105.0, 101.5, 2.5) === true, 'hit');
  check(r.stop === 102.5, `102.5 (${r.stop})`);
});

section('ChandelierExit short mirrors the long', () => {
  const r = new lrvx.ChandelierExit(0, 3.0);
  r.arm('short', 100.0, 2.0);
  check(r.stop === 106.0, `106 (${r.stop})`);
  check(r.update(101.0, 97.0, 2.0) === false && r.stop === 103.0, `103 (${r.stop})`);
  check(r.update(99.0, 94.0, 2.0) === false && r.stop === 100.0, `100 (${r.stop})`);
  check(r.update(100.5, 96.0, 2.0) === true, 'high 100.5 reaches 100');
});

section('BreakEvenTrail moves to entry on the trigger then trails', () => {
  const r = new lrvx.BreakEvenTrail(2.0, 3.0, 1.5);
  r.arm('long', 100.0, 2.0);
  check(r.stop === 96.0 && r.atBreakEven === false, `96, not at break-even (${r.stop})`);
  check(r.update(103.0, 99.0, 2.0) === false, 'bar 0');
  check(r.stop === 96.0 && r.atBreakEven === false, `still 96 (${r.stop})`);
  check(r.update(106.0, 102.0, 2.0) === false, 'bar 1');
  check(r.atBreakEven === true && r.stop === 103.0, `at break-even, 103 (${r.stop})`);
  check(r.update(110.0, 105.0, 2.5) === false, 'bar 2');
  check(r.stop === 106.25, `106.25 (${r.stop})`);
  check(r.update(108.0, 104.0, 2.5) === true, 'low 104 reaches 106.25');
  check(r.stop === 106.25, `106.25 (${r.stop})`);
  r.disarm();
  check(r.armed === false && r.atBreakEven === false && Number.isNaN(r.stop), 'disarm clears');
});

section('PercentStop: a fixed percentage from the entry', () => {
  throws(() => new lrvx.PercentStop(0), RangeError, 'pct 0 rejected');
  throws(() => new lrvx.PercentStop(-5), RangeError, 'negative pct rejected');
  throws(() => new lrvx.PercentStop(NaN), RangeError, 'NaN pct rejected');
  const s = new lrvx.PercentStop(5);
  check(s.pct === 5, `pct ${s.pct}`);
  check(s.armed === false && s.update(201.0, 150.0) === false, 'nothing is watched before arm');
  s.arm('long', 200.0);
  check(s.armed === true && s.stop === 190.0, `long stop 190 (${s.stop})`);
  check(s.update(215.0, 195.0) === false, 'low 195 above 190');
  check(s.update(212.0, 190.0) === true, 'low == stop is a hit');
  check(s.update(212.0, 190.0) === true, 'a hit does not disarm');
  check(s.update(212.0, 191.0) === false && s.armed === true,
        'per bar: a later bar that does not reach the stop is no hit, still armed');
  check(s.stop === 190.0, `the stop does not move (${s.stop})`);
  s.arm('short', 200.0);
  check(s.stop === 210.0, `short stop 210 (${s.stop})`);
  check(s.update(209.0, 195.0) === false, 'high 209 below 210');
  check(s.update(210.0, 201.0) === true, 'high == stop is a hit');
  s.disarm();
  check(s.armed === false && Number.isNaN(s.stop), `disarm: stop NaN (${s.stop})`);
  check(s.update(300.0, 1.0) === false, 'after disarm nothing is watched');
  throws(() => s.arm('sell', 200.0), RangeError, "side 'sell' is rejected");
  // The formula pinned on every side: entry * (1 +/- pct / 100).
  const f = new lrvx.PercentStop(2.5);
  f.arm('short', 100.0);
  check(f.stop === 100.0 * (1 + 2.5 / 100), `short 102.49999999999999 (${f.stop})`);
  f.arm('long', 100.0);
  check(f.stop === 97.5, `long 97.5 (${f.stop})`);
});

section('TakeProfit: a fixed percentage in the position\'s favour', () => {
  throws(() => new lrvx.TakeProfit(0), RangeError, 'pct 0 rejected');
  throws(() => new lrvx.TakeProfit(-1), RangeError, 'negative pct rejected');
  const t = new lrvx.TakeProfit(25);
  check(t.armed === false && t.update(1000.0, 1.0) === false, 'nothing is watched before arm');
  t.arm('long', 200.0);
  check(t.target === 250.0, `long target 250 (${t.target})`);
  check(t.update(249.0, 190.0) === false, 'high 249 below 250; a low far below is no hit');
  check(t.update(250.0, 205.0) === true, 'high == target is a hit');
  check(t.update(250.0, 205.0) === true, 'a hit does not disarm');
  check(t.update(240.0, 205.0) === false && t.armed === true,
        'per bar: a later bar below the target is no hit, still armed');
  t.arm('short', 200.0);
  check(t.target === 150.0, `short target 150 (${t.target})`);
  check(t.update(195.0, 151.0) === false, 'low 151 above 150');
  check(t.update(190.0, 150.0) === true, 'low == target is a hit');
  t.disarm();
  check(t.armed === false && Number.isNaN(t.target), `disarm: target NaN (${t.target})`);
});

section('TimeStop: hit on the maxBars-th update', () => {
  throws(() => new lrvx.TimeStop(0), RangeError, 'maxBars 0 rejected');
  const t = new lrvx.TimeStop(3);
  check(t.maxBars === 3, `maxBars ${t.maxBars}`);
  check(t.armed === false && t.update() === false && t.bars === 0, 'nothing counted before arm');
  t.arm();
  check(t.armed === true && t.bars === 0, 'armed, 0 bars');
  check(t.update() === false && t.bars === 1, 'update 1');
  check(t.update() === false && t.bars === 2, 'update 2');
  check(t.update() === true && t.bars === 3, 'update 3 is the hit');
  check(t.update() === true && t.bars === 4, 'a hit does not disarm');
  t.disarm();
  check(t.armed === false && t.bars === 0, 'disarm resets');
  t.arm();
  check(t.update() === false && t.bars === 1, 're-arm restarts the count');
  const one = new lrvx.TimeStop(1);
  one.arm();
  check(one.update() === true, 'maxBars 1: the first update hits');
});

section('every property reads back what it was built with', () => {
  // Distinct values, so a property that reads another parameter shows.
  const a = new lrvx.AtrStop(2.5);
  check(a.atrMultiple === 2.5, `AtrStop.atrMultiple ${a.atrMultiple}`);
  check(a.armed === false, `AtrStop.armed ${a.armed}`);
  check(Number.isNaN(a.stop), `AtrStop.stop before arm ${a.stop}`);
  a.arm('long', 100.0, 2.0);
  check(a.stop === 95.0, `AtrStop.stop after arm ${a.stop}`);
  check(a.atrMultiple === 2.5, `AtrStop.atrMultiple after arm ${a.atrMultiple}`);

  const c = new lrvx.ChandelierExit(4, 3.5);
  check(c.lookback === 4, `ChandelierExit.lookback ${c.lookback}`);
  check(c.atrMultiple === 3.5, `ChandelierExit.atrMultiple ${c.atrMultiple}`);
  check(c.armed === false, `ChandelierExit.armed ${c.armed}`);
  check(Number.isNaN(c.stop), `ChandelierExit.stop before arm ${c.stop}`);
  c.arm('long', 100.0, 2.0);
  check(c.stop === 93.0, `ChandelierExit.stop after arm ${c.stop}`);

  const b = new lrvx.BreakEvenTrail(2.0, 3.0, 1.5);
  check(b.armed === false && b.atBreakEven === false, 'BreakEvenTrail before arm');
  check(Number.isNaN(b.stop), `BreakEvenTrail.stop before arm ${b.stop}`);
  b.arm('long', 100.0, 2.0);
  check(b.stop === 96.0 && b.armed === true && b.atBreakEven === false,
        `BreakEvenTrail after arm: stop ${b.stop}`);

  const p = new lrvx.PercentStop(4);
  check(p.pct === 4, `PercentStop.pct ${p.pct}`);
  check(p.armed === false && Number.isNaN(p.stop), `PercentStop before arm: ${p.stop}`);
  const t = new lrvx.TakeProfit(30);
  check(t.pct === 30, `TakeProfit.pct ${t.pct}`);
  check(t.armed === false && Number.isNaN(t.target), `TakeProfit before arm: ${t.target}`);
  const ts = new lrvx.TimeStop(7);
  check(ts.maxBars === 7, `TimeStop.maxBars ${ts.maxBars}`);
  check(ts.armed === false && ts.bars === 0, `TimeStop before arm: bars ${ts.bars}`);
});

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed > 0 ? 1 : 0);
