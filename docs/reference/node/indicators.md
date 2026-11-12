# Indicators

```javascript
const lrvx = require('@lrvx/lrvx');
```

---

## Batch functions

Input arrays are `Float64Array`. Single-output functions return `Float64Array`.

```javascript
const ema  = lrvx.ema(closes, 20);
const macd = lrvx.macd(closes, 12, 26, 9);       // { line, signal, histogram }
const bb   = lrvx.bollinger(closes, 20, 2.0);     // { upper, middle, lower }
const st   = lrvx.stochastic(hi, lo, cl, 14, 3); // { k, d }
const adx  = lrvx.adx(hi, lo, cl, 14);           // { adx, plusDi, minusDi }
```

**Single value** — `(input, period)`:

`sma`, `ema`, `rma`, `rsi`, `dema`, `tema`, `slope`

`kama(input, period, fast?, slow?)` takes two extra optional smoothing constants.

**OHLC input:**

`atr(high, low, close, period)`, `cci(high, low, close, period)`, `chop(high, low, close, period)`,
`parkinson_vol(high, low, period)`, `rogers_satchell_vol(open, high, low, close, period)`

**Statistical** — `(input, period)`:

`skewness`, `kurtosis`, `rolling_zscore`, `shannon_entropy(input, period, bins)`, `rollingCorrelation(x, y, period)`

`lrvx.correlation(x, y)` (no `period`) is the single-number Pearson coefficient — see [Statistics](profiles.md#statistics).

**Volume:**

`obv(close, volume)`, `vwap(close, volume, window)`, `cvd(open, high, low, close, volume)`

---

## Streaming classes

All streaming indicators share the same interface:

```javascript
const ind = new lrvx.EMA(14);
ind.update(price);   // returns current value (null during warmup)
ind.value            // current value
ind.ready            // true once warmed up
ind.reset()          // clear state, keep config
```

**Single value** — `update(value)`:

`SMA(period)`, `EMA(period)`, `RMA(period)`, `RSI(period)`, `DEMA(period)`, `TEMA(period)`,
`KAMA(period, fast?, slow?)`, `Slope(length)`, `Skewness(period)`, `Kurtosis(period)`,
`RollingZScore(period)`, `ShannonEntropy(period, bins)`, `AutoCorrelation(window, lag)`

**Multi-output** — `update(value)`, named properties instead of `.value`:

`MACD(fast?, slow?, signal?)` — defaults 12/26/9 → `.line`, `.signal`, `.histogram`  
`Bollinger(period, stdDev?)` — default stdDev 2.0 → `.upper`, `.middle`, `.lower`

**OHLC / multi-input:**

`ATR(period)` — `update(high, low, close)`  
`Stochastic(kPeriod, dPeriod?)` — `update(high, low, close)` → `.k`, `.d`  
`CCI(period)` — `update(high, low, close)`  
`CHOP(period)` — `update(high, low, close)`  
`ParkinsonVol(period)` — `update(high, low)`  
`RogersSatchellVol(period)` — `update(open, high, low, close)`  
`Correlation(period)` — `update(x, y)`

Volume indicators (`obv`, `vwap`, `cvd`) exist as batch functions only — there are no streaming classes for them.

## Indicator catalog

<!-- INDICATOR-LIST-START -->

Every indicator below is **one Node.js class** with both a batch
`compute()` method and streaming `update()` / `value` / `ready` / `reset()`.
Same instance, two ways to use it:

```js
const lrvx = require('@lrvx/lrvx');
const ema = new lrvx.EMA(10);
const out = ema.compute(prices);            // batch
for (const v of stream) {
  ema.update(v);
  if (ema.ready) console.log(ema.value);    // streaming on the same instance
}
```

| Indicator | Constructor | Kind |
|---|---|---|
| `EMA` | `new lrvx.EMA(period)` | SingleInput |
| `SMA` | `new lrvx.SMA(period)` | SingleInput |
| `RMA` | `new lrvx.RMA(period)` | SingleInput |
| `RSI` | `new lrvx.RSI(period)` | SingleInput |
| `KAMA` | `new lrvx.KAMA(period, fast, slow)` | SingleInput |
| `DEMA` | `new lrvx.DEMA(period)` | SingleInput |
| `TEMA` | `new lrvx.TEMA(period)` | SingleInput |
| `Slope` | `new lrvx.Slope(length)` | SingleInput |
| `Skewness` | `new lrvx.Skewness(period)` | SingleInput |
| `Kurtosis` | `new lrvx.Kurtosis(period)` | SingleInput |
| `RollingZScore` | `new lrvx.RollingZScore(period)` | SingleInput |
| `ShannonEntropy` | `new lrvx.ShannonEntropy(period, bins)` | SingleInput |
| `AutoCorrelation` | `new lrvx.AutoCorrelation(window, lag)` | SingleInput |
| `ATR` | `new lrvx.ATR(period)` | BarInput |
| `CCI` | `new lrvx.CCI(period)` | BarInput |
| `CHOP` | `new lrvx.CHOP(period)` | BarInput |
| `Stochastic` | `new lrvx.Stochastic(k_period, d_period)` | BarInput |
| `ParkinsonVol` | `new lrvx.ParkinsonVol(period)` | HighLowInput |
| `RogersSatchellVol` | `new lrvx.RogersSatchellVol(period)` | OhlcInput |
| `Correlation` | `new lrvx.Correlation(period)` | PairInput |
| `MACD` | `new lrvx.MACD(fast, slow, signal)` | MultiOutput |
| `Bollinger` | `new lrvx.Bollinger(period, stddev)` | MultiOutput |


<!-- INDICATOR-LIST-END -->
