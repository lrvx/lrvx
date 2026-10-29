# Indicators

Vectorized technical indicators operating on numpy arrays. All functions release the GIL for parallel computation.

## Streaming classes

All streaming indicators share the same interface:

```python
ind = lrvx.EMA(20)
result = ind.update(price)  # None during warmup, float when ready
ind.value                   # None or float
ind.ready                   # bool
ind.reset()                 # clear state, keep config
ind.count                   # number of samples fed so far
```

During warmup, `update()` and `.value` return `None`. Check with `if result is not None` or use `.ready`.

**Single value** — `update(value) -> float | None`:

`SMA(period)`, `EMA(period)`, `RMA(period)`, `RSI(period)`, `DEMA(period)`, `TEMA(period)`, `KAMA(period, fast=2, slow=30)`, `Slope(length)`, `Skewness(period)`, `Kurtosis(period)`, `RollingZScore(period)`, `ShannonEntropy(period, bins=10)`, `AutoCorrelation(window, lag)`

**Multi-output** — `update(value) -> float | None`, named properties instead of `.value`:

`MACD(fast=12, slow=26, signal=9)` → `.line`, `.signal`, `.histogram`  
`Bollinger(period=20, multiplier=2.0)` → `.upper`, `.middle`, `.lower`

**OHLC / multi-input:**

`ATR(period)` — `update(high, low, close)`  
`Stochastic(k_period=14, d_period=3)` — `update(high, low, close)` → `.k`, `.d`  
`CCI(period=20)` — `update(high, low, close)`  
`ParkinsonVol(period)` — `update(high, low)`  
`RogersSatchellVol(period)` — `update(open, high, low, close)`  
`Correlation(period)` — `update(x, y)`

Volume indicators (`obv`, `vwap`, `cvd`) exist as batch functions only — there are no streaming classes for them.

---

## Batch functions

## Moving Averages

### `ema(input, period) -> ndarray`

Exponential Moving Average.

```python
result = lrvx.ema(closes, period=20)
```

### `sma(input, period) -> ndarray`

Simple Moving Average.

```python
result = lrvx.sma(closes, period=20)
```

### `rma(input, period) -> ndarray`

Wilder's Moving Average (alpha = 1/period). Used internally by RSI and ATR.

```python
result = lrvx.rma(closes, period=14)
```

### `dema(input, period) -> ndarray`

Double EMA: `2*EMA - EMA(EMA)`. Reduces lag versus standard EMA.

```python
result = lrvx.dema(closes, period=20)
```

### `tema(input, period) -> ndarray`

Triple EMA: `3*EMA - 3*EMA(EMA) + EMA(EMA(EMA))`. Further lag reduction.

```python
result = lrvx.tema(closes, period=20)
```

### `kama(input, period=10) -> ndarray`

Kaufman Adaptive Moving Average. Adjusts smoothing based on market efficiency.

```python
result = lrvx.kama(closes, period=10)
```

---

## Oscillators

### `rsi(input, period) -> ndarray`

Relative Strength Index (0–100).

```python
result = lrvx.rsi(closes, period=14)
```

### `macd(input, fast=12, slow=26, signal=9) -> dict`

Moving Average Convergence Divergence. Returns a dict with three arrays.

```python
result = lrvx.macd(closes, fast=12, slow=26, signal=9)
# result['line'], result['signal'], result['histogram']
```

### `stochastic(high, low, close, k_period=14, d_period=3) -> dict`

Stochastic oscillator (%K and %D).

```python
result = lrvx.stochastic(highs, lows, closes, k_period=14, d_period=3)
k = result['k']
d = result['d']
```

### `cci(high, low, close, period=20) -> ndarray`

Commodity Channel Index.

```python
result = lrvx.cci(highs, lows, closes, period=20)
```

---

## Trend

### `adx(high, low, close, period=14) -> dict`

Average Directional Index with directional indicators.

```python
result = lrvx.adx(highs, lows, closes, period=14)
# result['adx'], result['plus_di'], result['minus_di']
```

### `chop(high, low, close, period=14) -> ndarray`

Choppiness Index (0–100). High values indicate ranging markets.

```python
result = lrvx.chop(highs, lows, closes, period=14)
```

### `slope(input, length=1) -> ndarray`

Linear slope over a lookback window.

```python
result = lrvx.slope(closes, length=5)
```

---

## Volatility

### `atr(high, low, close, period) -> ndarray`

Average True Range.

```python
result = lrvx.atr(highs, lows, closes, period=14)
```

### `bollinger(input, period=20, stddev=2.0) -> dict`

Bollinger Bands.

```python
result = lrvx.bollinger(closes, period=20, stddev=2.0)
# result['upper'], result['middle'], result['lower']
```

---

## Statistical

### `skewness(input, period) -> ndarray`

Rolling Fisher-Pearson skewness. Measures distribution asymmetry. Requires period >= 3. NaN if std = 0.

```python
result = lrvx.skewness(closes, period=20)
```

### `kurtosis(input, period) -> ndarray`

Rolling Fisher excess kurtosis. Measures tail heaviness. Requires period >= 4. NaN if std = 0.

```python
result = lrvx.kurtosis(closes, period=20)
```

### `rolling_zscore(input, period) -> ndarray`

Rolling z-score normalization: `(x - mean) / std`. NaN if std = 0.

```python
result = lrvx.rolling_zscore(closes, period=20)
```

### `shannon_entropy(input, period, bins=10) -> ndarray`

Rolling Shannon entropy with histogram binning, normalized to [0, 1]. Zero = all values identical, 1 = uniform distribution.

```python
result = lrvx.shannon_entropy(closes, period=20, bins=10)
```

### `parkinson_vol(high, low, period) -> ndarray`

Parkinson high-low volatility estimator: `sqrt(mean(ln(H/L)^2) / (4*ln(2)))`. More efficient than close-to-close volatility.

```python
result = lrvx.parkinson_vol(highs, lows, period=20)
```

### `rogers_satchell_vol(open, high, low, close, period) -> ndarray`

Rogers-Satchell OHLC volatility estimator. Unbiased with drift, suitable for trending markets.

```python
result = lrvx.rogers_satchell_vol(opens, highs, lows, closes, period=20)
```

### `rolling_correlation(x, y, period) -> ndarray`

Rolling Pearson correlation between two series. NaN if either series is constant within the window.

`lrvx.correlation(x, y)` (no `period`) is the single-number Pearson coefficient — see [Optimizer](optimizer.md#correlation).

```python
result = lrvx.rolling_correlation(closes_btc, closes_eth, period=20)
```

### `autocorrelation(input, window, lag) -> ndarray`

Rolling autocorrelation of a series against itself at a fixed `lag`.

```python
result = lrvx.autocorrelation(closes, window=100, lag=1)
```

---

## Series diagnostics

### `adf(input, max_lag=4, regression='c') -> dict`

Augmented Dickey-Fuller unit-root test. `regression` selects the deterministic terms. Returns `test_stat`, `p_value`, `used_lag`.

```python
result = lrvx.adf(spread, max_lag=4)
if result['p_value'] < 0.05:
    print("spread is stationary")
```

### `hurst_dfa(returns, scales=None) -> float`

Hurst exponent via Detrended Fluctuation Analysis. Input is a series of **returns**, not prices. `H ~ 0.5` random, `H > 0.5` persistent/trending, `H < 0.5` mean-reverting. `scales=None` uses a log-spaced grid from 4 to `N/4`. Returns NaN for inputs shorter than 32 or degenerate ones.

```python
h = lrvx.hurst_dfa(np.diff(np.log(closes)))
```

### `rolling_hurst(prices, window=1080) -> ndarray`

Rolling DFA-Hurst on price levels. For bar `i` it computes the Hurst exponent of the previous `window` log-returns; `output[i]` is NaN for `i <= window`. Output length matches input.

```python
result = lrvx.rolling_hurst(closes, window=1080)
```

### `realized_vol(returns, periods_per_year) -> float`

Annualized realized volatility from log returns: sample stdev times `sqrt(periods_per_year)`.

```python
rv = lrvx.realized_vol(log_returns, periods_per_year=365 * 24)
```

### `whites_reality_check(returns, num_bootstrap=10000, avg_block_size=0.0, seed=42) -> dict`

White's reality check via stationary bootstrap. Tests whether the best of `K` candidate strategies beats zero after multiple-comparison correction. `returns` is a `(K, T)` array of **excess** returns relative to a benchmark. Returns `p_value`, `best_stat`, `best_index`.

```python
result = lrvx.whites_reality_check(excess_returns)
print(result['best_index'], result['p_value'])
```

### `pbo_cscv(returns, num_blocks=16) -> dict`

Probability of backtest overfitting by combinatorially symmetric cross-validation. `returns` is a `(trials, periods)` array of per-period returns, one row per parameter combination; `num_blocks` is the even number of period blocks the `C(S, S/2)` splits are drawn from. Returns `pbo` (the fraction of splits whose in-sample best ranks below the out-of-sample median; NaN when the inputs cannot be split), `splits`, `mean_oos_rank` and `mean_logit`. Deterministic.

### `deflated_sharpe(returns, trial_sharpes=None) -> dict`

Deflated Sharpe ratio. `returns` is the selected strategy's per-period return series; `trial_sharpes` holds the per-period Sharpe ratio of every trial the selection ran over. Returns `sharpe`, `skewness`, `kurtosis` (3 for normal returns), `expected_max_sharpe` (0 without trials) and `dsr`, the probability that the true Sharpe ratio exceeds the expected maximum of that many unskilled trials.

### `grid_neighborhood(values, radius=1) -> dict`

The neighbourhood of every point of a parameter grid. `values` is an n-dimensional array of one metric per grid point. Returns `neighbor_mean` (the mean over the cells whose index differs by at most `radius` on every axis, the cell itself excluded, NaN values skipped; NaN with no neighbour) and `neighbor_count`, both shaped like `values`.

### `list_indicators() -> list`

Names of every indicator class registered in this build.

---

## Volume

### `vwap(close, volume, window=96) -> ndarray`

Rolling Volume-Weighted Average Price.

```python
result = lrvx.vwap(closes, volumes, window=96)
```

### `obv(close, volume) -> ndarray`

On-Balance Volume.

```python
result = lrvx.obv(closes, volumes)
```

### `cvd(open, high, low, close, volume) -> ndarray`

Cumulative Volume Delta. Estimates buying/selling pressure from OHLCV data.

```python
result = lrvx.cvd(opens, highs, lows, closes, volumes)
```

---

## Strategy Helpers

### `bar_returns(signal_long, signal_short, log_returns) -> ndarray`

Compute per-bar returns given position signals and log returns.

```python
returns = lrvx.bar_returns(signal_long, signal_short, log_returns)
```

`signal_long` and `signal_short` are `int8[]` (+1 or 0 / -1 or 0). `log_returns` is `float64[]`.

### `trade_pnl(signal_long, signal_short, log_returns) -> ndarray`

Extract per-trade PnL from position signals.

```python
pnls = lrvx.trade_pnl(signal_long, signal_short, log_returns)
```

### `profit_factor(returns) -> float`

Ratio of gross profit to gross loss. Values > 1.0 indicate profitability.

```python
pf = lrvx.profit_factor(returns)
```

### `win_rate(trade_pnls) -> float`

Fraction of trades with positive PnL.

```python
wr = lrvx.win_rate(pnls)
```

## Indicator catalog

<!-- INDICATOR-LIST-START -->

Every indicator below is **one Python class** with both a batch
`compute()` method and streaming `update()`/`value`/`ready`/`reset()`.
Same instance, two ways to use it:

```python
import lrvx
ema = lrvx.EMA(10)
out = ema.compute(prices)             # batch
for v in stream:
    ema.update(v)
    if ema.ready: print(ema.value)    # streaming on the same instance
```

| Indicator | Constructor | Kind |
|---|---|---|
| `EMA` | `lrvx.EMA(period)` | SingleInput |
| `SMA` | `lrvx.SMA(period)` | SingleInput |
| `RMA` | `lrvx.RMA(period)` | SingleInput |
| `RSI` | `lrvx.RSI(period)` | SingleInput |
| `KAMA` | `lrvx.KAMA(period, fast, slow)` | SingleInput |
| `DEMA` | `lrvx.DEMA(period)` | SingleInput |
| `TEMA` | `lrvx.TEMA(period)` | SingleInput |
| `Slope` | `lrvx.Slope(length)` | SingleInput |
| `Skewness` | `lrvx.Skewness(period)` | SingleInput |
| `Kurtosis` | `lrvx.Kurtosis(period)` | SingleInput |
| `RollingZScore` | `lrvx.RollingZScore(period)` | SingleInput |
| `ShannonEntropy` | `lrvx.ShannonEntropy(period, bins)` | SingleInput |
| `AutoCorrelation` | `lrvx.AutoCorrelation(window, lag)` | SingleInput |
| `ATR` | `lrvx.ATR(period)` | BarInput |
| `CCI` | `lrvx.CCI(period)` | BarInput |
| `Stochastic` | `lrvx.Stochastic(k_period, d_period)` | BarInput |
| `ParkinsonVol` | `lrvx.ParkinsonVol(period)` | HighLowInput |
| `RogersSatchellVol` | `lrvx.RogersSatchellVol(period)` | OhlcInput |
| `Correlation` | `lrvx.Correlation(period)` | PairInput |
| `MACD` | `lrvx.MACD(fast, slow, signal)` | MultiOutput |
| `Bollinger` | `lrvx.Bollinger(period, stddev)` | MultiOutput |

Discovery: `lrvx.list_indicators()` returns the full list at runtime.

<!-- INDICATOR-LIST-END -->
