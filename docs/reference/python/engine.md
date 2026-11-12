# Engine & Backtest

The core backtest engine. Load OHLCV data once, build a signal set, then run. Bars are merged across symbols by timestamp; market orders fill at the bar close.

## Engine

```python
import lrvx

engine = lrvx.Engine(initial_capital=100_000, fee_rate=0.0001)
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `initial_capital` | `float` | `100000.0` | Starting capital |
| `fee_rate` | `float` | `0.0001` | Fee rate per trade (percentage mode) |

### Loading

Each loader registers (or overwrites) one named symbol. When `symbol` is omitted, `load_csv` infers the name from the filename and `load_ohlcv` / `load_df` use `"default"`. Timestamps are auto-normalized to nanoseconds (seconds, milliseconds and microseconds are detected by magnitude).

#### `load_csv(path, symbol='')`

Load bars from a headed CSV with columns `timestamp,open,high,low,close,volume` in that order. The symbol name is derived from the file stem (uppercased, with a trailing `_1m` / `_5m` / `_15m` / `_1h` / `_4h` / `_1d` stripped) unless `symbol` is given.

```python
engine.load_csv("./data/btcusdt_1h.csv")            # symbol -> "BTCUSDT"
engine.load_csv("./data/eth.csv", symbol="ETHUSDT")
```

Raises `LrvxError(code="E_IO_001")` if the file cannot be opened.

#### `load_ohlcv(data, symbol='')`

Load bars from a dict of numpy arrays.

```python
engine.load_ohlcv({
    "ts": timestamps,     # int64 — 'timestamp' is also accepted
    "open": opens,
    "high": highs,
    "low": lows,
    "close": closes,
    "volume": volumes,
}, symbol="BTCUSDT")
```

| Key | Type | Description |
|-----|------|-------------|
| `ts` or `timestamp` | `int64[]` | Bar timestamps (s, ms, us or ns — auto-detected) |
| `open` | `float64[]` | Open prices |
| `high` | `float64[]` | High prices |
| `low` | `float64[]` | Low prices |
| `close` | `float64[]` | Close prices |
| `volume` | `float64[]` | Volume |

A missing key raises `LrvxError(code="E_KEY_001")`.

#### `load_df(df, symbol='')`

Load bars from a pandas DataFrame with `open`, `high`, `low`, `close`, `volume` columns. The timestamp column is the first of `ts`, `timestamp`, `open_time`, `time` that exists; if none does, the DataFrame index is used.

```python
engine.load_df(df, symbol="BTCUSDT")
```

#### `resample(symbol, target, interval)`

Resample an already-loaded symbol into a new symbol named `target`.

```python
engine.load_csv("./data/btcusdt_1m.csv", symbol="BTC_1M")
engine.resample("BTC_1M", "BTC_1H", "1h")
```

| Parameter | Type | Description |
|-----------|------|-------------|
| `symbol` | `str` | Source symbol name |
| `target` | `str` | Name of the resampled symbol |
| `interval` | `str` | Bucket size — number plus `s`, `m`, `h` or `d` |

An unknown unit raises `LrvxError(code="E_TIME_001")`.

### Running

#### `run(signals, default_symbol=0) -> Stats`

Run a backtest over every loaded symbol. `signals` is a [`SignalBuilder`](#signalbuilder). Signals built without an explicit symbol name are routed to `default_symbol` (a numeric symbol ID); `0` means the first loaded symbol.

```python
stats = engine.run(signals)
print(stats.net_pnl, stats["sharpe_ratio"])
```

Referencing an unregistered symbol name raises `LrvxError(code="E_SYM_001")`.

`Engine` symbols are names with no tick, so limit prices are not checked against one; a
`BacktestRunner` on a registry with a tick rejects an off-tick price
([SimulatedExecutor](../api/backtest/simulated_executor.md#order-prices-and-the-tick)).

### Accessors

| Member | Type | Description |
|--------|------|-------------|
| `symbols` | `list[str]` | Registered symbol names (property) |
| `bar_count(symbol='')` | `int` | Number of bars loaded for a symbol |
| `ts(symbol='')` | `int64[]` | Bar timestamps in nanoseconds |
| `open(symbol='')` | `float64[]` | Open prices |
| `high(symbol='')` | `float64[]` | High prices |
| `low(symbol='')` | `float64[]` | Low prices |
| `close(symbol='')` | `float64[]` | Close prices |
| `volume(symbol='')` | `float64[]` | Volume |

An empty `symbol` selects the first symbol loaded. Calling any accessor before loading data raises `LrvxError(code="E_RUN_002")`.

---

## SignalBuilder

Accumulates the orders a backtest should submit. Timestamps are normalized the same way as bar timestamps; each signal fires on the first bar whose timestamp is at or past it.

```python
signals = lrvx.SignalBuilder()
signals.buy(ts_ns, 1.0)
signals.limit_sell(ts_ns, 51_000.0, 1.0, "BTCUSDT")
len(signals)
```

| Method | Description |
|--------|-------------|
| `buy(ts, qty, symbol='')` | Market buy |
| `sell(ts, qty, symbol='')` | Market sell |
| `limit_buy(ts, price, qty, symbol='')` | Limit buy |
| `limit_sell(ts, price, qty, symbol='')` | Limit sell |
| `clear()` | Drop all accumulated signals |
| `__len__()` | Signal count |

`symbol` is a registered symbol name. Left empty, the signal is routed to `run()`'s `default_symbol`.

---

## Stats

Returned by `Engine.run()`. Fields are readable as attributes or by key, and `to_dict()` returns the whole set as a plain dict.

```python
stats = engine.run(signals)

stats.sharpe_ratio    # attribute access
stats["sharpe_ratio"]  # key access
stats.to_dict()       # dict of every field
repr(stats)           # Stats(trades=... pnl=... ret=...% sharpe_ratio=... dd=...%)
```

| Field | Type | Description |
|-------|------|-------------|
| `total_trades` | `int` | Round-trip trade count |
| `winning_trades` | `int` | Profitable trade count |
| `losing_trades` | `int` | Losing trade count |
| `initial_capital` | `float` | Starting capital |
| `final_capital` | `float` | Ending capital |
| `total_pnl` | `float` | Gross PnL |
| `total_fees` | `float` | Total fees paid |
| `net_pnl` | `float` | PnL after fees |
| `gross_profit` | `float` | Sum of winning trades |
| `gross_loss` | `float` | Sum of losing trades |
| `max_drawdown` | `float` | Maximum drawdown (absolute) |
| `max_drawdown_pct` | `float` | Maximum drawdown (percentage) |
| `win_rate` | `float` | Fraction of winning trades |
| `profit_factor` | `float` | Gross profit / gross loss |
| `avg_win` | `float` | Average winning trade |
| `avg_loss` | `float` | Average losing trade |
| `sharpe_ratio` | `float` | Annualized Sharpe ratio |
| `sortino_ratio` | `float` | Annualized Sortino ratio |
| `calmar_ratio` | `float` | Calmar ratio |
| `return_pct` | `float` | Net return percentage |

`BacktestResult.stats()` in [Backtest Components](backtest.md#backtestresult) returns a dict with a different, larger key set — the ratios are named `sharpe_ratio` / `sortino_ratio` / `calmar_ratio` there.

---

## Example

```python
import numpy as np
import lrvx

n = 500
rng = np.random.default_rng(42)
ts = 1_700_000_000_000_000_000 + np.arange(n, dtype=np.int64) * 3_600_000_000_000
close = 30_000.0 + np.cumsum(rng.normal(0.0, 25.0, n))

engine = lrvx.Engine(initial_capital=100_000, fee_rate=0.0001)
engine.load_ohlcv({
    "ts": ts,
    "open": close,
    "high": close + 5.0,
    "low": close - 5.0,
    "close": close,
    "volume": np.full(n, 1.0),
}, symbol="BTCUSDT")

closes = engine.close("BTCUSDT")
fast = lrvx.ema(closes, 10)
slow = lrvx.ema(closes, 30)

signals = lrvx.SignalBuilder()
for i in range(1, n):
    if fast[i] > slow[i] and fast[i - 1] <= slow[i - 1]:
        signals.buy(int(ts[i]), 1.0, "BTCUSDT")
    elif fast[i] < slow[i] and fast[i - 1] >= slow[i - 1]:
        signals.sell(int(ts[i]), 1.0, "BTCUSDT")

stats = engine.run(signals)
print(f"Signals: {len(signals)}, trades: {stats.total_trades}")
print(f"Net PnL: {stats.net_pnl:.2f}, Sharpe: {stats.sharpe_ratio:.4f}")
```
