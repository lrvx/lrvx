# Parameter optimization with grid search

Find good strategy parameters by sweeping a grid and ranking results. The pattern is the same in every binding: define a grid, run a backtest per combination, rank by your chosen metric.

## Quick start

=== "Python"

    ```python
    import lrvx
    import pandas as pd

    def run_one(fast: int, slow: int) -> dict:
        reg = lrvx.SymbolRegistry()
        btc = reg.add_symbol("binance", "BTCUSDT", tick_size=0.01)
        strat = SmaCrossover([btc], fast=fast, slow=slow)
        bt = lrvx.BacktestRunner(reg, fee_rate=0.0004, initial_capital=10_000)
        bt.set_strategy(strat)
        return bt.run_csv("data/btcusdt_1m.csv", "BTCUSDT")

    rows = []
    for fast in [5, 10, 15, 20]:
        for slow in [20, 30, 40, 50]:
            if fast >= slow:
                continue
            stats = run_one(fast, slow)
            rows.append({"fast": fast, "slow": slow, **stats})

    df = pd.DataFrame(rows).sort_values("sharpe_ratio", ascending=False)
    print(df.head())
    df.to_csv("grid_results.csv", index=False)
    ```

    For parallelism, wrap `run_one` in `multiprocessing.Pool` or `concurrent.futures.ProcessPoolExecutor`. lrvx releases the GIL during the C++ backtest, so threads work too.

=== "Node.js"

    ```javascript
    const lrvx = require('@lrvx/lrvx');

    function runOne(fast, slow) {
      const reg = new lrvx.SymbolRegistry();
      const btc = reg.addSymbol("binance", "BTCUSDT", 0.01);
      const strat = new SmaCrossover([btc], fast, slow);
      const bt = new lrvx.BacktestRunner(reg, 0.0004, 10_000);
      bt.setStrategy(strat);
      return bt.runCsv("data/btcusdt_1m.csv", "BTCUSDT");
    }

    const rows = [];
    for (const fast of [5, 10, 15, 20]) {
      for (const slow of [20, 30, 40, 50]) {
        if (fast >= slow) continue;
        rows.push({ fast, slow, ...runOne(fast, slow) });
      }
    }
    rows.sort((a, b) => b.sharpeRatio - a.sharpeRatio);
    console.log(rows.slice(0, 5));
    ```

=== "C++"

    ```cpp
    #include "lrvx/backtest/backtest_optimizer.h"
    #include "lrvx/backtest/optimization_stats.h"

    struct MAParams {
      int fastPeriod, slowPeriod;
      std::string toString() const {
        return "fast=" + std::to_string(fastPeriod) + ",slow=" + std::to_string(slowPeriod);
      }
    };

    struct MAGrid {
      std::vector<int> fastPeriods = {5, 10, 15, 20};
      std::vector<int> slowPeriods = {20, 30, 40, 50};
      size_t totalCombinations() const { return fastPeriods.size() * slowPeriods.size(); }
      MAParams operator[](size_t i) const {
        return { fastPeriods[i / slowPeriods.size()], slowPeriods[i % slowPeriods.size()] };
      }
    };

    BacktestOptimizer<MAParams, MAGrid> optimizer;
    optimizer.setParameterGrid(MAGrid{});
    optimizer.setBacktestFactory([&](const MAParams& p) { return runBacktest(p); });
    auto results = optimizer.runLocal();
    auto ranked  = BacktestOptimizer<MAParams, MAGrid>::rankResults(results, RankMetric::SharpeRatio);
    std::cout << "Best: " << ranked[0].parameters.toString() << "\n";
    ```

`runLocal(numThreads)` runs the combinations on that many threads, one per hardware thread when it is 0, and returns the results in grid order. The factory is called from several threads at once, so it builds everything a backtest touches per call and shares only read-only inputs, such as a bar vector loaded once. The type-erased `GridSearch::run(numThreads)` works the same way from C++.

## Ranking metrics

Keys below are for the dict returned by `BacktestRunner.run_csv` / `run_ohlcv` / `run_bars` / `run_tape` / `run_tapes`, which is what the examples on this page rank on.

| Metric | Python | Node.js | C++ (`RankMetric::*`) |
|---|---|---|---|
| Sharpe | `sharpe_ratio` | `sharpeRatio` | `SharpeRatio` |
| Sortino | `sortino_ratio` | `sortinoRatio` | `SortinoRatio` |
| Calmar | `calmar_ratio` | `calmarRatio` | `CalmarRatio` |
| Total return | `return_pct` | `returnPct` | `TotalReturn` |
| Max drawdown | `max_drawdown_pct` | `maxDrawdownPct` | `MaxDrawdown` |
| Win rate | `win_rate` | `winRate` | `WinRate` |
| Profit factor | `profit_factor` | `profitFactor` | `ProfitFactor` |

All three ratios are on this dict. `BacktestResult.stats()` carries the same names plus the trade-duration, streak and TWR fields — see [Two stats shapes, one set of key names](backtest.md#two-stats-shapes-one-set-of-key-names).

## Filtering, stability, statistical tests

The C++ `BacktestOptimizer` ships ranking, filtering, bootstrap CIs, and permutation tests as templated utilities (see [`optimization_stats.h`](../reference/api/backtest/optimization_stats.md)). The same primitives are exposed as free functions in `lrvx` — prefer them over hand-rolling with `scipy`:

=== "Python"

    ```python
    import numpy as np
    import lrvx

    sharpes = df["sharpe_ratio"].values
    returns = df["return_pct"].values
    print("mean Sharpe:", sharpes.mean(), " std:", sharpes.std())
    print("corr(Sharpe, return):", np.corrcoef(sharpes, returns)[0, 1])

    # Bootstrap 95% CI for mean Sharpe -> (lower, median, upper)
    print("CI:", lrvx.bootstrap_ci(sharpes, confidence=0.95, num_samples=10_000))

    # Two-sample permutation test: top-10 vs bottom-10 -> p-value
    top, bot = np.sort(sharpes)[-10:], np.sort(sharpes)[:10]
    print("p =", lrvx.permutation_test(top, bot, num_permutations=10_000))

    # Multiple-comparison correction across K candidate strategies.
    # excess: shape (K, T) of EXCESS returns -> {p_value, best_stat, best_index}
    print(lrvx.whites_reality_check(excess, num_bootstrap=10_000))
    ```

    `lrvx` also ships `profit_factor(returns)`, `win_rate(trade_pnls)` and
    `trade_pnl(signal_long, signal_short, log_returns)`. Node.js has the same set
    camelCased — `bootstrapCI`, `permutationTest`, `whitesRealityCheck`,
    `profitFactor`, `winRate`, `tradePnl` — but `whitesRealityCheck` takes a flat
    row-major matrix plus `numStrategies` / `numPeriods` rather than a 2-D array.

=== "C++"

    ```cpp
    using Stats = OptimizationStatistics<MAParams, MAGrid>;
    Stats::printSummary(results);
    auto sharpes = extractMetric(results, RankMetric::SharpeRatio);
    auto ci      = Stats::bootstrapCI(sharpes, 0.95, 10000);     // .lower / .median / .upper
    auto pValue  = Stats::permutationTest(group1, group2, 10000);
    bool wrote   = Stats::generateReport(results, "report.md");  // false if the path can't be opened
    ```

    `generateReport` prints the Drawdown column straight from
    `max_drawdown_pct`, which is already a percent -- 10.00% means ten
    percent of the account. The Win Rate column beside it is scaled from a
    fraction, which is why only one of the two carries a `* 100`.

    `bootstrapCI` and `permutationTest` resample with a fixed default seed
    (`42`), so the same inputs always return the same result; pass a trailing
    `seed` argument for an independent resample. See
    [`optimization_stats.h`](../reference/api/backtest/optimization_stats.md)
    for the full signatures.

## Best practices

1. **Avoid overfitting** — every extra parameter increases overfit risk
2. **Walk-forward** — optimise on train, validate on a held-out test slice
3. **Parameter stability** — the best point should have good neighbours, not be an isolated peak
4. **Realistic costs** — include slippage and exchange fees
5. **Statistical significance** — bootstrap CI / permutation tests separate luck from edge

## Type-erased `GridSearch` class (Python / Node / Codon)

The template-based `BacktestOptimizer<ParamsT, GridT>` shown above is C++-only. For the language bindings, lrvx exposes a type-erased `GridSearch` class that takes axes of `double` values and a factory callback. Last axis varies fastest (row-major flatten).

### Python

```python
import lrvx

reg = lrvx.SymbolRegistry()
btc = reg.add_symbol("exchange", "BTCUSDT", 0.01)


def factory(params):
    fast, slow = int(params[0]), int(params[1])
    if fast >= slow:
        return {"sharpe_ratio": 0.0, "return_pct": 0.0, "total_trades": 0}

    class _S(lrvx.Strategy):
        def __init__(self, syms):
            super().__init__(syms)
            self.fast = lrvx.SMA(fast)
            self.slow = lrvx.SMA(slow)
        def on_trade(self, ctx, t):
            f = self.fast.update(t.price); s = self.slow.update(t.price)
            if f is None or s is None or not self.slow.ready: return
            if f > s and ctx.is_flat(): self.market_buy(0.01)
            elif f < s and ctx.is_long(): self.market_sell(0.01)

    bt = lrvx.BacktestRunner(reg, 0.0004, 10_000)
    bt.set_strategy(_S([btc]))
    return bt.run_csv("data/btcusdt_sample.csv", symbol="BTCUSDT")


gs = lrvx.GridSearch()
gs.add_axis([5.0, 10.0, 20.0])
gs.add_axis([30.0, 50.0, 100.0])
gs.set_factory(factory)
for r in gs.run():
    fast, slow = r["params"]
    s = r["stats"]
    print(f"fast={int(fast):3d} slow={int(slow):3d}: "
          f"return={s['return_pct']:+.4f}% sharpe={s['sharpe_ratio']:+.4f}")
```

The factory takes `list[float]` and returns the same dict shape as `BacktestRunner.run_csv`.

### Node

```js
const gs = new lrvx.GridSearch();
gs.addAxis([5, 10, 20]);
gs.addAxis([30, 50, 100]);
gs.setFactory((params) => {
  // ... return camelCase BacktestStats: { returnPct, sharpeRatio, totalTrades, ... }
});
const results = gs.run();  // [{index, params, stats}, ...]
```

### Pairs with walk-forward

Compose with [walk-forward](walk-forward.md): run a `GridSearch` on each fold's train slice, pick the best params by your criterion, evaluate on test. The two primitives stay separate so the choice of "best" stays opinionated to the caller (sharpe vs. profit factor vs. drawdown-aware).

### Limitations

`GridSearch.run()` in Python, Node and Codon runs the combinations one after another: the Python factory holds the GIL while it builds and runs the strategy, and a Node factory is a JavaScript function on the one JavaScript thread. A parallel sweep from Python uses the process pool shown in the quick start; from C++, `GridSearch::run(numThreads)` and `BacktestOptimizer::runLocal(numThreads)` run on threads. The result list is unsorted — sort or filter on the caller side.

## See also

- [Backtesting](./backtest.md)
- [Walk-forward](./walk-forward.md)
- [Optimization stats reference (C++)](../reference/api/backtest/optimization_stats.md)
- [Python optimizer reference](../reference/python/optimizer.md)
