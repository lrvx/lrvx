# Indicators

Technical indicators for Codon strategies. Two types:

1. **Batch** — compute over an entire array at once (calls C++ via C API)
2. **Streaming** — update one value at a time per tick (pure Codon, compiled to native)

---

## Batch indicators

Batch indicators are **free functions**, not class methods. Import them by name.

```codon
from lrvx.indicators import ema, sma, rsi, atr, macd, bollinger
from lrvx.indicators import skewness, kurtosis, rolling_zscore, shannon_entropy
from lrvx.indicators import parkinson_vol, rogers_satchell_vol, rolling_correlation, autocorrelation
from lrvx.indicators import obv, vwap, cvd, chop, adx, adf, stochastic
```

**Single value** — returns `List[float]`:

`ema(data, period)`, `sma(data, period)`, `rma(data, period)`, `rsi(data, period)`, `dema(data, period)`, `tema(data, period)`, `kama(data, period, fast=2, slow=30)`, `slope(data, length)`

**OHLC / multi-input** — returns `List[float]`:

`atr(high, low, close, period)`, `cci(high, low, close, period)`, `chop(high, low, close, period)`, `parkinson_vol(high, low, period)`, `rogers_satchell_vol(open_, high, low, close, period)`, `rolling_correlation(x, y, period)`

**Statistical** — returns `List[float]`:

`skewness(data, period)`, `kurtosis(data, period)`, `rolling_zscore(data, period)`, `shannon_entropy(data, period, bins=10)`, `autocorrelation(data, window, lag)`

**Volume** — returns `List[float]`:

`obv(close, volume)`, `vwap(close, volume, window)`, `cvd(open_, high, low, close, volume)`

**Multi-output:**

`macd(data, fast=12, slow=26, signal=9)` — returns `MacdResult`: `.line`, `.signal`, `.histogram`
`bollinger(data, period=20, multiplier=2.0)` — returns `BollingerResult`: `.upper`, `.middle`, `.lower`
`stochastic(high, low, close, k_period=14, d_period=3)` — returns `StochasticResult`: `.k`, `.d`
`adx(high, low, close, period=14)` — returns `AdxResult`: `.adx`, `.plus_di`, `.minus_di`
`adf(data, max_lag=4, regression="c")` — returns `AdfResult`: `.test_stat`, `.p_value`, `.used_lag` (scalars, not lists)

```codon
values = ema(prices, 20)

m = macd(prices, 12, 26, 9)
print(m.line[-1], m.signal[-1])

ranges = atr(highs, lows, closes, 14)

skew = skewness(prices, 20)
```

---

## Streaming indicators

All streaming indicators share the same pattern: call `update()` each tick, read `.value`, check `.ready`. All support `.reset()` to clear state, `.count` (updates since construction or the last reset) and `.close()` to release the C handle.

```codon
from lrvx.indicators import EMA, SMA, RSI, ATR, MACD, Bollinger
from lrvx.indicators import RMA, DEMA, TEMA, KAMA, Slope, CCI, Stochastic
from lrvx.indicators import Skewness, Kurtosis, RollingZScore, ShannonEntropy
from lrvx.indicators import ParkinsonVol, RogersSatchellVol, Correlation, AutoCorrelation
```

The complete class list is `SMA`, `EMA`, `RMA`, `DEMA`, `TEMA`, `KAMA`, `RSI`, `Slope`, `ATR`, `CCI`,
`MACD`, `Bollinger`, `Stochastic`, `Skewness`, `Kurtosis`, `RollingZScore`, `ShannonEntropy`,
`ParkinsonVol`, `RogersSatchellVol`, `Correlation`, `AutoCorrelation`. There is no `OBV`, `VWAP` or
`CVD` class — those three are batch-only free functions.

Each instance holds one C handle (`lrvx_streaming_indicator_*`) that owns the C++ streaming
indicator. `update()` runs one step of it and keeps no history, so the cost of an update depends
only on the indicator's window, not on how many values it has seen: O(1) for SMA, EMA, RMA, RSI,
DEMA, TEMA, Slope, ATR and MACD, O(period) for the others, which recompute their window each step.
`.value`, `.ready` and the NaN behaviour are the C++ class's, and `.value` after k updates equals
element k - 1 of the batch result bit for bit. `Stochastic.ready` needs both %K and %D.

`.close()` releases the handle at once. An instance that is never closed is released by `__del__`
when the collector reclaims it; one still reachable when the program exits is left to the process
exit. After `.close()`, `.value` is NaN, `.ready` is False and `.count` is 0.

### Single-price indicators

#### `EMA`

```codon
ema = EMA(period=20)
value = ema.update(price)
if ema.ready:
    print(ema.value)
```

#### `SMA`

A running sum over a circular buffer: O(1) per update.

```codon
sma = SMA(period=20)
value = sma.update(price)
```

#### `RMA`

Wilder's Moving Average (used internally by RSI and ATR).

```codon
rma = RMA(period=14)
value = rma.update(price)
```

#### `DEMA`

Double Exponential Moving Average. `.ready` is true after `2 * period` values.

```codon
dema = DEMA(period=20)
value = dema.update(price)
```

#### `TEMA`

Triple Exponential Moving Average. `.ready` is true after `3 * period` values.

```codon
tema = TEMA(period=20)
value = tema.update(price)
```

#### `KAMA`

Kaufman's Adaptive Moving Average.

```codon
kama = KAMA(period=10)
value = kama.update(price)
```

#### `Slope`

Linear regression slope over a rolling window.

```codon
slope = Slope(period=20)
value = slope.update(price)
```

#### `RSI`

```codon
rsi = RSI(period=14)
value = rsi.update(price)
if rsi.ready:
    print(rsi.value)  # 0..100
```

#### `Skewness`

Fisher-Pearson skewness. Requires period >= 3.

```codon
skew = Skewness(period=20)
value = skew.update(price)
```

#### `Kurtosis`

Fisher excess kurtosis. Requires period >= 4.

```codon
kurt = Kurtosis(period=20)
value = kurt.update(price)
```

#### `RollingZScore`

`(x - mean) / std`.

```codon
zscore = RollingZScore(period=20)
value = zscore.update(price)
```

#### `ShannonEntropy`

Rolling Shannon entropy, normalized to [0, 1].

```codon
ent = ShannonEntropy(period=20, bins=10)
value = ent.update(price)
```

---

### Multi-value indicators

#### `ATR`

```codon
atr = ATR(period=14)
value = atr.update(high, low, close)
```

#### `MACD`

```codon
macd = MACD(fast=12, slow=26, signal=9)
macd.update(price)
if macd.ready:
    print(macd.line, macd.signal, macd.histogram)
```

#### `Bollinger`

```codon
bb = Bollinger(period=20, multiplier=2.0)
bb.update(price)
if bb.ready:
    print(bb.upper, bb.middle, bb.lower)
```

#### `ParkinsonVol`

Parkinson high-low volatility estimator.

```codon
pvol = ParkinsonVol(period=20)
value = pvol.update(high, low)
```

#### `RogersSatchellVol`

Rogers-Satchell OHLC volatility estimator.

```codon
rsv = RogersSatchellVol(period=20)
value = rsv.update(open_, high, low, close)
```

#### `Correlation`

Rolling Pearson correlation between two series.

```codon
corr = Correlation(period=20)
value = corr.update(x, y)
```

#### `CCI`

Commodity Channel Index.

```codon
cci = CCI(period=20)
value = cci.update(high, low, close)
```

#### `Stochastic`

```codon
st = Stochastic(k_period=14, d_period=3)
st.update(high, low, close)
if st.ready:
    print(st.k, st.d)
```

#### `AutoCorrelation`

Rolling autocorrelation at a fixed lag. `.ready` is true after `window + lag` values.

```codon
ac = AutoCorrelation(window=50, lag=1)
value = ac.update(price)
```

---

### Volume indicators

There are no streaming volume-indicator classes. On-Balance Volume, VWAP and Cumulative Volume Delta
are batch-only free functions:

```codon
from lrvx.indicators import obv, vwap, cvd

obv_series  = obv(closes, volumes)
vwap_series = vwap(closes, volumes, 20)
cvd_series  = cvd(opens, highs, lows, closes, volumes)
```

---

## Indicator catalog

<!-- INDICATOR-LIST-START -->

Every indicator below is **one Codon class** with streaming `update()` / `value` /
`ready` / `count` / `reset()`, one kernel step of the C++ class per update, and
`close()` to release its C handle. A batch `compute()` is **not** on every class — the `Batch`
column says what each class actually exposes. Classes and the batch free functions
both come from `lrvx.indicators`.

```codon
from lrvx.indicators import EMA, CCI, cci

ema = EMA(20)
out = ema.compute(prices)                  # EMA has an instance compute()
for v in stream:
    ema.update(v)
    if ema.ready: print(ema.value)         # streaming on the same instance

c = CCI(20)                                # CCI has no compute()
series = cci(highs, lows, closes, 20)      # batch via the free function
```

| Indicator | Constructor | Kind | Batch |
|---|---|---|---|
| `EMA` | `EMA(period)` | SingleInput | instance `compute(data)` |
| `SMA` | `SMA(period)` | SingleInput | instance `compute(data)`, static `compute_static(data, period)` |
| `RMA` | `RMA(period)` | SingleInput | instance `compute(data)` |
| `RSI` | `RSI(period)` | SingleInput | instance `compute(data)` |
| `KAMA` | `KAMA(period, fast=2, slow=30)` | SingleInput | instance `compute(data)` |
| `DEMA` | `DEMA(period)` | SingleInput | instance `compute(data)` |
| `TEMA` | `TEMA(period)` | SingleInput | instance `compute(data)` |
| `Slope` | `Slope(length)` | SingleInput | instance `compute(data)` |
| `Skewness` | `Skewness(period)` | SingleInput | none — free function `skewness(data, period)` |
| `Kurtosis` | `Kurtosis(period)` | SingleInput | none — free function `kurtosis(data, period)` |
| `RollingZScore` | `RollingZScore(period)` | SingleInput | none — free function `rolling_zscore(data, period)` |
| `ShannonEntropy` | `ShannonEntropy(period, bins=10)` | SingleInput | none — free function `shannon_entropy(data, period, bins=10)` |
| `AutoCorrelation` | `AutoCorrelation(window, lag)` | SingleInput | none — free function `autocorrelation(data, window, lag)` |
| `ATR` | `ATR(period)` | BarInput | instance `compute(h, l, c)` |
| `CCI` | `CCI(period)` | BarInput | none — free function `cci(high, low, close, period)` |
| `Stochastic` | `Stochastic(k_period=14, d_period=3)` | BarInput | none — free function `stochastic(high, low, close, k_period=14, d_period=3)` |
| `ParkinsonVol` | `ParkinsonVol(period)` | HighLowInput | none — free function `parkinson_vol(high, low, period)` |
| `RogersSatchellVol` | `RogersSatchellVol(period)` | OhlcInput | none — free function `rogers_satchell_vol(open_, high, low, close, period)` |
| `Correlation` | `Correlation(period)` | PairInput | none |
| `MACD` | `MACD(fast=12, slow=26, signal=9)` | MultiOutput | instance `compute(data)` |
| `Bollinger` | `Bollinger(period=20, multiplier=2.0)` | MultiOutput | instance `compute(data)` |

Streaming only, with no `compute()` at all — call the matching free function for batch:
`Skewness`, `Kurtosis`, `RollingZScore`, `ShannonEntropy`, `AutoCorrelation`, `CCI`,
`Stochastic`, `ParkinsonVol`, `RogersSatchellVol`, `Correlation`.

`SMA.compute_static(data, period)` is the only static batch method in the module.


<!-- INDICATOR-LIST-END -->
