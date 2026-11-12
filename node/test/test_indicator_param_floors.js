// node/test/test_indicator_param_floors.js
//
// Every registry indicator refuses integer arguments below their floor
// (include/lrvx/indicator/params.h): the constructor and the batch function
// both throw an Error with code E_IND_001. Before, they returned all-NaN
// arrays (RSI(0) and KAMA with a zero fast or slow length returned numbers),
// and ShannonEntropy with 0 bins crashed the process. At the floor nothing
// changes.

'use strict';

const lrvx = require('..');

let _failed = 0;
let _passed = 0;
function check(cond, msg)
{
  if (cond) {
    _passed++;
    console.log(`  ok  - ${msg}`);
  } else {
    _failed++;
    console.log(`  FAIL - ${msg}`);
  }
}

const N = 40;
const c = new Float64Array(N), h = new Float64Array(N), l = new Float64Array(N), o = new Float64Array(N);
let x = 100;
for (let i = 0; i < N; ++i) {
  x += Math.sin(i * 1.7) * 2 + 0.1;
  c[i] = x; h[i] = x + 1; l[i] = x - 1; o[i] = x + 0.3;
}

// name: [class, class args at the floor, batch(args) at the floor or null]
const ROWS = {
  ema: [lrvx.EMA, [1], (a) => lrvx.ema(c, a[0])],
  sma: [lrvx.SMA, [1], (a) => lrvx.sma(c, a[0])],
  rma: [lrvx.RMA, [1], (a) => lrvx.rma(c, a[0])],
  rsi: [lrvx.RSI, [1], (a) => lrvx.rsi(c, a[0])],
  kama: [lrvx.KAMA, [1, 1, 1], null],
  dema: [lrvx.DEMA, [1], (a) => lrvx.dema(c, a[0])],
  tema: [lrvx.TEMA, [1], (a) => lrvx.tema(c, a[0])],
  slope: [lrvx.Slope, [1], (a) => lrvx.slope(c, a[0])],
  skewness: [lrvx.Skewness, [3], (a) => lrvx.skewness(c, a[0])],
  kurtosis: [lrvx.Kurtosis, [4], (a) => lrvx.kurtosis(c, a[0])],
  rolling_zscore: [lrvx.RollingZScore, [2], (a) => lrvx.rolling_zscore(c, a[0])],
  shannon_entropy: [lrvx.ShannonEntropy, [2, 2], (a) => lrvx.shannon_entropy(c, a[0], a[1])],
  autocorrelation: [lrvx.AutoCorrelation, [2, 1], (a) => lrvx.autocorrelation(c, a[0], a[1])],
  atr: [lrvx.ATR, [1], (a) => lrvx.atr(h, l, c, a[0])],
  cci: [lrvx.CCI, [1], (a) => lrvx.cci(h, l, c, a[0])],
  chop: [lrvx.CHOP, [2], (a) => lrvx.chop(h, l, c, a[0])],
  stochastic: [lrvx.Stochastic, [1, 1], (a) => lrvx.stochastic(h, l, c, a[0], a[1])],
  parkinson_vol: [lrvx.ParkinsonVol, [1], (a) => lrvx.parkinson_vol(h, l, a[0])],
  rogers_satchell_vol: [lrvx.RogersSatchellVol, [1], (a) => lrvx.rogers_satchell_vol(o, h, l, c, a[0])],
  correlation: [lrvx.Correlation, [2], (a) => lrvx.rollingCorrelation(c, h, a[0])],
  macd: [lrvx.MACD, [1, 1, 1], (a) => lrvx.macd(c, a[0], a[1], a[2])],
  bollinger: [lrvx.Bollinger, [1], (a) => lrvx.bollinger(c, a[0], 2.0)],
};

function refused(fn, what)
{
  try {
    fn();
    check(false, `${what} throws`);
  } catch (e) {
    check(e.code === 'E_IND_001' && /must be at least/.test(e.message),
          `${what} throws E_IND_001 (${e.code}: ${e.message})`);
  }
}

function works(fn, what)
{
  try {
    fn();
    check(true, `${what} works`);
  } catch (e) {
    check(false, `${what} works (threw ${e.message})`);
  }
}

check(Object.keys(ROWS).length === 22, 'every registry row has a case');

for (const [name, [Cls, floor, batch]] of Object.entries(ROWS)) {
  for (let k = 0; k < floor.length; ++k) {
    const bad = floor.slice();
    bad[k] -= 1;
    refused(() => new Cls(...bad), `new ${name}(${bad})`);
    if (batch) {
      refused(() => batch(bad), `${name} batch (${bad})`);
    }
  }
  works(() => new Cls(...floor), `new ${name}(${floor})`);
  if (batch) {
    works(() => batch(floor), `${name} batch (${floor})`);
  }
}
refused(() => lrvx.kama(c, 0), 'kama batch (0)');

// The refused constructor left no half-built object behind.
const sma = new lrvx.SMA(3);
sma.update(1); sma.update(2);
check(sma.update(3) === 2, 'a valid SMA after the refusals still computes');

console.log(`\n${_passed} passed, ${_failed} failed`);
if (_failed > 0) {
  process.exit(1);
}
