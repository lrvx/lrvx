# Indicators

Every indicator class has `.update()` for per-tick use plus a **static** `.compute()` for batch, and
`.reset()` to clear state. Whether the *instance* also carries a `compute()` differs per class — the
[indicator catalog](#indicator-catalog) below lists it per class.

Classes are injected as globals by the embedded runtime. There is no `require()` in QuickJS.

```javascript
// Single value
const ema = new EMA(20);
ema.update(price);        // returns current value (null during warmup)
ema.count;                // updates since construction or the last reset()
ema.reset();
ema.destroy();            // optional: frees the C handle now instead of at GC

// Multi-output
const macd = new MACD(12, 26, 9);
macd.update(price);
macd.line; macd.signal; macd.histogram;

// OHLC input
const atr = new ATR(14);
atr.update(high, low, close);

const stoch = new Stochastic(14, 3);
stoch.update(high, low, close);
stoch.k; stoch.d;

// Multi-input
const corr = new Correlation(20);
corr.update(x, y);

const pvol = new ParkinsonVol(20);
pvol.update(high, low);

// Batch
const adxResult = ADX.compute(highs, lows, closes, 14);
adxResult.adx; adxResult.plusDi; adxResult.minusDi;

const skewArr = Skewness.compute(prices, 20);
```

**Cost and lifetime.** Each streaming instance holds one C handle (`lrvx_streaming_indicator_*`)
that owns the C++ streaming indicator. `update()` is one call that runs one step of it; no history
is kept in JavaScript or in C, so the cost of an update depends only on the indicator's window,
not on how many values it has seen: O(1) for SMA, EMA, RMA, RSI, DEMA, TEMA, Slope, ATR and MACD,
O(period) for the others. Values, `ready` and the NaN behaviour are the C++ class's, bit for bit,
with NaN shown as `null`; `Stochastic.ready` needs both `.k` and `.d`. `destroy()` frees the handle
at once and a second call does nothing; an instance dropped without it is freed by the QuickJS
finalizer when it is collected, and every remaining one when the runtime is torn down. After
`destroy()` the value reads `null`, `ready` is false and `count` is 0.

**Single value** — `update(value)`:

`SMA`, `EMA`, `RMA`, `DEMA`, `TEMA`, `KAMA`, `RSI`, `Slope`, `Skewness`, `Kurtosis`, `RollingZScore`, `ShannonEntropy`

**Multi-output** — named properties instead of `.value`:

`MACD` → `.line`, `.signal`, `.histogram`  
`Bollinger` → `.upper`, `.middle`, `.lower`

**OHLC / multi-input:**

`ATR`, `CCI`, `CHOP` — `update(high, low, close)`  
`Stochastic` — `update(high, low, close)` → `.k`, `.d`  
`ADX` — batch only: `ADX.compute(highs, lows, closes, period)` → `.adx`, `.plusDi`, `.minusDi`  
`ParkinsonVol` — `update(high, low)`  
`RogersSatchellVol` — `update(open, high, low, close)`  
`Correlation` — `update(x, y)`

**Volume — batch-only static helpers, no `update()` / `value` / `ready`:**

`OBV.compute(close, volume)`
`VWAP.compute(close, volume, window)`
`CVD.compute(open, high, low, close, volume)`

## Indicator catalog

<!-- INDICATOR-LIST-START -->

Every indicator below is a **global class**: the embedded runtime evaluates
`quickjs/lrvx/indicators.js` at global scope, so there is no `require()` and no
namespace prefix. Streaming is `update()` / `value` / `ready` / `count` /
`reset()` on the instance, one kernel step of the C++ class per update, and
`destroy()` releases the instance's C handle. Batch is a **static** `compute()`.
Only some classes also carry an instance `compute()` — see the `Batch` column.

```js
const ema = new EMA(10);                    // global class, no require()
const out = EMA.compute(prices, 10);        // batch: static compute()
for (const v of stream) {
  ema.update(v);
  if (ema.ready) console.log(ema.value);    // streaming on the instance
}
```

| Indicator | Constructor | Kind | Batch |
|---|---|---|---|
| `EMA` | `new EMA(period)` | SingleInput | static `EMA.compute(data, period)` |
| `SMA` | `new SMA(period)` | SingleInput | static `SMA.compute(data, period)` |
| `RMA` | `new RMA(period)` | SingleInput | static `RMA.compute(data, period)` |
| `RSI` | `new RSI(period)` | SingleInput | static `RSI.compute(data, period)` |
| `KAMA` | `new KAMA(period, fast=2, slow=30)` | SingleInput | static `KAMA.compute(data, period, fast=2, slow=30)`, instance `compute(data)` |
| `DEMA` | `new DEMA(period)` | SingleInput | static `DEMA.compute(data, period)` |
| `TEMA` | `new TEMA(period)` | SingleInput | static `TEMA.compute(data, period)` |
| `Slope` | `new Slope(length)` | SingleInput | static `Slope.compute(data, length)` |
| `Skewness` | `new Skewness(period)` | SingleInput | static `Skewness.compute(data, period)` |
| `Kurtosis` | `new Kurtosis(period)` | SingleInput | static `Kurtosis.compute(data, period)` |
| `RollingZScore` | `new RollingZScore(period)` | SingleInput | static `RollingZScore.compute(data, period)` |
| `ShannonEntropy` | `new ShannonEntropy(period, bins=10)` | SingleInput | static `ShannonEntropy.compute(data, period, bins=10)`, instance `compute(data)` |
| `AutoCorrelation` | `new AutoCorrelation(window, lag)` | SingleInput | static `AutoCorrelation.compute(data, window, lag)`, instance `compute(data)` |
| `ATR` | `new ATR(period)` | BarInput | static `ATR.compute(high, low, close, period)` |
| `CCI` | `new CCI(period)` | BarInput | static `CCI.compute(high, low, close, period)` |
| `CHOP` | `new CHOP(period)` | BarInput | static `CHOP.compute(high, low, close, period)` |
| `Stochastic` | `new Stochastic(kPeriod=14, dPeriod=3)` | BarInput | static `Stochastic.compute(high, low, close, kPeriod=14, dPeriod=3)`, instance `compute(high, low, close)` |
| `ParkinsonVol` | `new ParkinsonVol(period)` | HighLowInput | static `ParkinsonVol.compute(high, low, period)` |
| `RogersSatchellVol` | `new RogersSatchellVol(period)` | OhlcInput | static `RogersSatchellVol.compute(open, high, low, close, period)` |
| `Correlation` | `new Correlation(period)` | PairInput | static `Correlation.compute(x, y, period)` |
| `MACD` | `new MACD(fast=12, slow=26, signal=9)` | MultiOutput | static `MACD.compute(data, fast=12, slow=26, signal=9)`, instance `compute(data)` |
| `Bollinger` | `new Bollinger(period=20, multiplier=2.0)` | MultiOutput | static `Bollinger.compute(data, period=20, multiplier=2.0)`, instance `compute(data)` |

An instance `compute()` exists only on `KAMA`, `ShannonEntropy`, `AutoCorrelation`,
`Stochastic`, `MACD`, `Bollinger`. On every other class above `new EMA(20).compute(prices)` is
`undefined` — call `EMA.compute(prices, 20)`.

Also defined in `lrvx/indicators.js` but not in the shared registry: `ADX`, `OBV`, `VWAP`, `CVD`
(batch-only static `compute()`, no `update()`).


<!-- INDICATOR-LIST-END -->
