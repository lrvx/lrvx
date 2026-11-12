# Indicators

lrvx has 22 streaming indicator classes plus several batch-only functions, split across moving averages, oscillators, trend, volatility, volume, and statistics. The same set is exposed by every binding — Python, Node.js, Codon, and the C++ core all share one implementation.

Most of them work in two modes: batch (pass an array, get an array back) and streaming (call `.update()` each tick, check `.ready` before reading `.value`). Several are batch-only — a free function but no indicator class or streaming mode: **ADX**, **OBV**, **VWAP**, **CVD**, plus the statistical **ADF** stationarity test and **Hurst/DFA** (`hurst_dfa`, `rolling_hurst`). The 22 streaming classes are the ones in `include/lrvx/indicator/registry.def`; `lrvx.list_indicators()` returns exactly that list at runtime.

This page covers what each one actually measures and when you'd want it.

---

## Moving averages

### SMA

Arithmetic mean over a sliding window. Every bar gets equal weight.

```
SMA(n) = (x₁ + x₂ + ... + xₙ) / n
```

Responds slowly to recent price movement, which makes it less useful for short-term signals but reasonable as a long-term trend reference. If you need a baseline to compare against, this is the simplest one.

### EMA

Weighted average where recent bars count more. Smoothing factor: `α = 2/(period+1)`.

```
EMA = α × price + (1 − α) × EMA_prev
```

Responds faster than SMA. MACD, ATR smoothing, and most other indicators build on it. The default choice when you need a moving average and have no strong reason to pick something else.

### RMA

Same structure as EMA but `α = 1/period` — slower. RSI and ATR use it internally (it's Wilder's smoothing).

You probably won't use RMA directly unless you're reimplementing RSI/ATR from scratch or need exact TradingView parity.

### DEMA

```
DEMA = 2 × EMA − EMA(EMA)
```

Less lag than EMA. Warmup takes `2 × period` bars. Worth trying when EMA crossover signals are consistently arriving a bar or two late.

### TEMA

```
TEMA = 3 × EMA − 3 × EMA(EMA) + EMA(EMA(EMA))
```

More lag reduction than DEMA. Warmup is `3 × period`. Very reactive — expect more false signals in choppy markets.

### KAMA

Kaufman Adaptive Moving Average. Adjusts the smoothing factor based on an "efficiency ratio": how much price moved versus how much it oscillated. Goes fast in trending markets, slow in sideways ones.

Useful if you want one MA that self-adjusts across regimes rather than manually switching between a fast and a slow EMA.

### Slope

Two-point rate of change over a rolling window, not a linear regression: the difference between the current value and the value `length` bars back, divided by the window length.

```
slope = (x[i] − x[i − length]) / length
```

This is cheaper than an OLS fit and reacts only to the window's two endpoints, ignoring the points between them. On a clean straight line the two formulas agree, so the difference is easy to miss in a simple example; on a noisy series they can diverge by 50% or more, since the two-point version does not average away the noise the way a regression slope does.

Positive = upward trend. Magnitude is the steepness. Good for momentum filtering.

---

## Oscillators

### RSI

Ratio of average gains to average losses over `period` bars, scaled to 0–100.

```
RSI = 100 − 100 / (1 + RS)
RS  = avg_gain / avg_loss   (Wilder's RMA)
```

The classic levels (70 = overbought, 30 = oversold) work well in ranging markets. In a strong trend, RSI can stay above 70 for a long time; whether that helps or hurts depends on your strategy. Period 14 is standard; shorter periods make it noisier.

A window with zero gain and zero loss (the instrument hasn't moved at all) reports the neutral 50, not 100: on a flat market, "maximally overbought" would be a misleading signal. A genuine uptrend (all gains) still reports 100, and a decline followed by a flat plateau still reports 0.

### MACD

Difference between a fast EMA and a slow EMA, with a signal line on top.

```
MACD line   = EMA(fast) − EMA(slow)    [default: 12, 26]
signal line = EMA(MACD line, 9)
histogram   = MACD line − signal line
```

Crossing zero signals a momentum shift. The histogram slope shows whether momentum is accelerating or fading. Watching the histogram flatten before the line crossover is a common entry filter.

### Stochastic

Where is the close relative to the recent high-low range?

```
%K = 100 × (close − lowest_low) / (highest_high − lowest_low)
%D = SMA(%K, d_period)
```

Ranges 0–100. Common levels: 80 overbought, 20 oversold. The %D line smooths %K. Main signals are the %K/%D crossover and divergence between price and the indicator.

### CCI

Distance of the typical price from its SMA, divided by mean absolute deviation.

```
TP  = (high + low + close) / 3
CCI = (TP − SMA(TP)) / (0.015 × mean_deviation)
```

Scaled so roughly 70% of values fall between −100 and +100. Values outside that band signal unusual strength or weakness. Often used as a momentum filter rather than a primary entry signal.

### Bollinger Bands

SMA with bands at ±N standard deviations.

```
middle = SMA(price, period)
upper  = middle + multiplier × std(price, period)
lower  = middle − multiplier × std(price, period)
```

Bands widen in volatile markets and contract when price quiets down. The squeeze — when bands get unusually narrow — often comes before a directional move. Price touching a band is context, not a signal by itself.

---

## Trend

### ADX

*Batch-only — `adx(high, low, close, period)`. No streaming class.*

Measures trend strength, not direction. Comes with two directional indicators.

```
+DI  = Wilder(upward movement, period)
−DI  = Wilder(downward movement, period)
ADX  = Wilder(|+DI − −DI| / (+DI + −DI), period)
```

ADX above 25 typically means there's a trend worth following. Below 20 is choppy. +DI and −DI tell you direction; ADX tells you whether to care.

### CHOP

*Streaming class `CHOP(period)` with `update(high, low, close)`, and batch `chop(high, low, close, period)`.*

How much price moved as a fraction of the maximum possible range over the period.

```
CHOP = 100 × log₁₀(Σ ATR / (highest_high − lowest_low)) / log₁₀(period)
```

High CHOP (near 100) means directionless. Low (near 0) means trending. More useful for switching between strategy modes than as a signal itself.

---

## Volatility

### ATR

Average range per bar, accounting for gaps.

```
true_range = max(high − low, |high − prev_close|, |low − prev_close|)
ATR        = RMA(true_range, period)
```

Not directional. Standard use: position sizing (stop = N × ATR from entry) and filtering signals by volatility regime.

### Parkinson volatility

Uses high-low ranges instead of close-to-close returns.

```
σ = sqrt(mean(ln(H/L)²) / (4 × ln(2)))
```

More efficient than close-to-close when you have intraday OHLC data. Underestimates if the market gaps frequently, since gaps don't show in the H-L range.

### Rogers-Satchell volatility

OHLC volatility estimator designed to handle drift (trending markets) without bias.

```
σ² = mean(ln(H/C) × ln(H/O) + ln(L/C) × ln(L/O))
```

Better than Parkinson for trending assets. Both can be annualized by multiplying by `sqrt(periods_per_year)`.

---

## Volume

### OBV

*Batch-only — `obv(close, volume)`. No streaming class.*

Running total: add volume on up bars, subtract on down bars.

```
OBV = OBV_prev + (close > prev_close ? +volume : −volume)
```

The absolute value is meaningless — you're looking at the trend of OBV and divergences from price. Price makes a new high but OBV doesn't: the rally may not have conviction.

### VWAP

*Batch-only — `vwap(close, volume, window)`. No streaming class.*

Average price weighted by volume, over a rolling window.

```
VWAP = Σ(price × volume) / Σ(volume)
```

Price above VWAP = buyers have been in control over that window. Used as a fair-value reference and order execution benchmark. Institutions care about VWAP when filling large orders.

### CVD

*Batch-only — `cvd(open, high, low, close, volume)`. No streaming class.*

Running total of buying minus selling volume, inferred from OHLCV.

```
CVD = CVD_prev + buy_volume − sell_volume
```

Similar to OBV but directional. Divergence between CVD and price is one of the more reliable short-term signals when it appears.

---

## Statistical

### Rolling z-score

How many standard deviations is the current value from the rolling mean?

```
z = (x − mean(x, period)) / std(x, period)
```

Standard use is mean-reversion signals: z > 2 or < −2 marks statistically unusual levels. Returns NaN when std = 0.

### Skewness

Fisher-Pearson skewness of a rolling window — measures distribution asymmetry.

```
skew = (n / ((n−1)(n−2))) × Σ((x − mean) / std)³
```

Positive = right tail (large gains skew the distribution). Negative = left tail. Common in volatility forecasting and regime detection. Requires period ≥ 3; NaN if std = 0.

### Kurtosis

Tail heaviness relative to a normal distribution (Fisher excess kurtosis, so a normal distribution = 0).

High kurtosis means fat tails: more outliers than a normal distribution would predict. Used in risk models to understand tail exposure. Requires period ≥ 4; NaN if std = 0.

### Shannon entropy

How random is the recent price distribution? Normalized to [0, 1] using histogram binning.

```
H = −Σ p(x) × ln(p(x)) / ln(bins)
```

1 = uniform distribution (maximum uncertainty). 0 = all values identical. Entropy tends to drop before trends develop and rise in choppy, uncertain markets. That makes it usable as a regime filter.

### Autocorrelation

Rolling Pearson correlation of a series with itself at a fixed lag.

```
AC(window, lag) = corr(x[t], x[t − lag])   over the trailing `window`
```

Constructed as `AutoCorrelation(window, lag)`. Positive values mean the series continues in the same direction at that lag (momentum); negative values mean it reverses (mean reversion). Useful for picking a holding horizon or deciding whether a series is worth a momentum model at all. Returns NaN when the window is constant.

### Correlation

Rolling Pearson correlation between two series.

```
r = Σ(x − x̄)(y − ȳ) / sqrt(Σ(x − x̄)² × Σ(y − ȳ)²)
```

Range [−1, 1]. Used for pairs construction, cross-asset filters, or detecting when a relationship is breaking down. Returns NaN when either series is constant within the window.

For the cross-symbol case (`Correlation(BTC, ETH)` from a live strategy or paired bar arrays), the engine does **not** auto-align timestamps — the caller must synchronise the streams first. See [the cross-symbol how-to](../how-to/cross-symbol-indicators.md) for the alignment recipe.
