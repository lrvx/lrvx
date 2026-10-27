# Sweep parameters over bars

Run one bar strategy over many parameter combinations fast enough to iterate on the grid, not wait for it.

## Load the bars once

`run_bars` takes the bar arrays and nothing else: no file, no parsing, no conversion. Load them once and hand the same arrays to every run. Timestamps are nanoseconds and the call does no unit guessing, so a file with millisecond timestamps is multiplied by 1 000 000 at load time, once.

- From CSV: `numpy.loadtxt` into one array, then one column per field.
- From Parquet: `pandas.read_parquet(path)`, then `.to_numpy()` on each column.
- From a recorded tape: [aggregate bars](bar-aggregation.md) once and keep the result.

`run_csv` and `run_ohlcv` read and parse their file on every call, which is the right tool for one run and the wrong one for a sweep.

## Run on bars

A bar strategy implements `on_bar(ctx, bar)` and reads `bar.open`, `bar.high`, `bar.low`, `bar.close`, `bar.volume`, `bar.start_time_ns` and `bar.end_time_ns`. The engine walks each bar (open, low, high, close) before the callback, holds every order the callback submits, and fills it at the next bar's open. The full set of rules, with pinned numbers, is on [the reference backtest](../explanation/reference-backtest.md) page.

Two exits that look alike are not: `market_sell(qty)` is a plain sell of `qty`, which reduces a long by that much and goes short past it; `close_position()` sells exactly the open position, reduce-only, and submits nothing when flat.

Keep the work per bar small. The streaming indicators (`lrvx.SMA`, `lrvx.EMA`, `lrvx.ATR` and the rest) update in place with no per-bar allocation, and a strategy that keeps only its indicators and its state costs the same on bar 3000 as on bar 40.

## The sweep

A combination is a fresh registry, runner and strategy; only the bar arrays are shared. The example below runs a small grid over the data file that ships in `docs/examples/data/`.

```python
--8<-- "examples/python_sweep_over_bars.py"
```

Processes, not threads: a Python strategy runs under the GIL, so two threads take turns on the callbacks rather than run at once. Each worker process loads the bars the first time it runs a combination and keeps them; the tasks it receives are parameters only.

From C++, `BacktestOptimizer::runLocal(numThreads)` and `GridSearch::run(numThreads)` run the combinations on threads, with the bar vector loaded once and shared read-only; see [grid search](grid-search.md).

## After the sweep

The result list is unsorted. Rank it on the caller side, then:

- hold out time with [walk-forward](walk-forward.md),
- separate luck from edge with the [statistical tests](grid-search.md#filtering-stability-statistical-tests), and
- prefer a parameter point with good neighbours over an isolated peak.

## See also

- [Grid search](grid-search.md)
- [The reference backtest](../explanation/reference-backtest.md)
- [Bar aggregation](bar-aggregation.md)
- [Backtesting](backtest.md)
