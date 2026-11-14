## Walk-forward backtesting

Walk-forward splits the time series into successive train / test
windows, runs the strategy through each pair, and returns per-fold
stats. Useful as a sanity check on overfitting: if the strategy looks
great on the train half but degrades on the held-out test half across
folds, the backtest is overfit.

`lrvx.WalkForwardRunner` ships in Python. The same primitive is in
`lrvx.WalkForwardRunner` for Node and through the C ABI for Codon
(`lrvx_walk_forward_run_csv`, and `lrvx_walk_forward_run_csv_result` for
the folds together with each leg's open position).

## Modes

`anchored` — the train window starts at bar 0 and grows. Each fold
trains on `[0, t]`, tests on `[t, t + test_size]`, then `t` advances by
`step`. Set `min_train_size` to skip the first folds where the train
window is too small.

`sliding` — the train window is fixed-size and slides forward. Each
fold trains on `[t, t + train_size]`, tests on
`[t + train_size, t + train_size + test_size]`, `t` advances by `step`.

## Python

```python
import lrvx

reg = lrvx.SymbolRegistry()
btc = reg.add_symbol("exchange", "BTCUSDT", 0.01)


class SmaCross(lrvx.Strategy):
    def __init__(self, syms):
        super().__init__(syms)
        self.fast = lrvx.SMA(10)
        self.slow = lrvx.SMA(30)

    def on_trade(self, ctx, t):
        f = self.fast.update(t.price)
        s = self.slow.update(t.price)
        if f is None or s is None or not self.slow.ready:
            return
        if f > s and ctx.is_flat():
            self.market_buy(0.01)
        elif f < s and ctx.is_long():
            self.close_position()


wfr = lrvx.WalkForwardRunner(
    reg, fee_rate=0.0004, initial_capital=10_000,
    mode="anchored", test_size=100, step=100, min_train_size=100,
)

# Factory called twice per fold (train, then test). Build a fresh
# strategy every time — state from a prior fold must not leak.
wfr.set_strategy_factory(lambda fold_index: SmaCross([btc]))

folds = wfr.run_csv("data/btcusdt_sample.csv", "BTCUSDT")
for f in folds:
    print(f"fold {f['fold_index']}: "
          f"train return={f['train_stats']['return_pct']:+.4f}% "
          f"sharpe={f['train_stats']['sharpe_ratio']:+.4f} | "
          f"test return={f['test_stats']['return_pct']:+.4f}% "
          f"sharpe={f['test_stats']['sharpe_ratio']:+.4f}")
```

The factory pattern is non-negotiable: the engine calls it once per
window with no shortcut for "reuse my strategy". A leaked indicator
buffer or position counter from a prior fold would silently corrupt
the next fold's stats.

## Node

```js
const lrvx = require('@lrvx/lrvx');

const reg = new lrvx.SymbolRegistry();
const btc = reg.addSymbol('exchange', 'BTCUSDT', 0.01);

const wfr = new lrvx.WalkForwardRunner(reg, 0.0004, 10000, {
  mode: 'anchored', testSize: 100, step: 100, minTrainSize: 100,
});

wfr.setStrategyFactory((foldIndex) => {
  const fast = new lrvx.SMA(10);
  const slow = new lrvx.SMA(30);
  return {
    symbols: [Number(btc)],
    onTrade(ctx, t, emit) {
      const f = fast.update(t.price);
      const s = slow.update(t.price);
      if (f === null || s === null || !slow.ready) return;
      if (f > s && ctx.position === 0) emit.marketBuy(0.01);
      else if (f < s && ctx.position > 0) emit.closePosition();
    },
  };
});

const folds = wfr.runCsv('data/btcusdt_sample.csv', 'BTCUSDT');
folds.forEach(f => console.log(f.foldIndex, f.testStats.returnPct));
```

`runCsv` throws an `LrvxError` with the reason when the CSV cannot be opened,
a row cannot be parsed or the symbol is not registered. An empty array means
the file was read and is too short for one fold.

## What you get back per fold

```
{
  "fold_index": 0,
  "train_start_bar": 0, "train_end_bar": 100,
  "test_start_bar": 100, "test_end_bar": 200,
  "train_start_ns": ..., "train_end_ns": ...,
  "test_start_ns": ..., "test_end_ns": ...,
  "train_stats": { ... full BacktestStats ... },
  "test_stats":  { ... full BacktestStats ... },
}
```

The two `*_stats` blocks are the same shape as `BacktestRunner.run_csv`
returns. Each values the position still open at the end of its own window,
marked at that window's last close: `unrealized_pnl` (`unrealizedPnl` in
Node) is `(mark - entry) * qty` for a long and `(entry - mark) * qty` for a
short, with no exit fee, and `final_equity` (`finalEquity`) is
`initial_capital + net_pnl + unrealized_pnl`. `net_pnl` and `final_capital`
stay the closed trades'. Compute aggregate statistics (mean / median / variance over
folds) on the client side — the runner does not aggregate for you on
purpose, since useful aggregates depend on what you are looking for
(robustness vs. average performance vs. worst case).

### C API

`LrvxWalkForwardFold` has no field for the valuation, and its shape is
fixed by the ABI. `lrvx_walk_forward_run_csv_result` runs the same folds as
`lrvx_walk_forward_run_csv` and keeps them in an owned handle:

```c
LrvxWalkForwardResultHandle r = lrvx_walk_forward_run_csv_result(
    registry, "data/btcusdt_sample.csv", "BTCUSDT", 0.0004, 10000.0, &cfg,
    factory, user_data);
if (r == NULL) { /* the reason is in lrvx_last_error_message() */ }

uint32_t n = lrvx_walk_forward_result_folds(r, NULL, 0);  /* the count */
LrvxWalkForwardFold* folds = malloc(n * sizeof *folds);
lrvx_walk_forward_result_folds(r, folds, n);
for (uint32_t i = 0; i < n; ++i)
{
  double open_at_test_end = lrvx_walk_forward_result_test_unrealized_pnl(r, i);
  double equity_at_test_end = lrvx_walk_forward_result_test_final_equity(r, i);
  /* _train_unrealized_pnl / _train_final_equity for the train leg */
}
free(folds);
lrvx_walk_forward_result_destroy(r);
```

It fails where `lrvx_walk_forward_run_csv` does -- a CSV that cannot be
opened or parsed, a symbol the registry does not hold, a NULL registry,
path, config or factory -- with NULL and the reason in
`lrvx_last_error_message()`. A file too short for one fold is a handle with
zero folds and no error. A `fold_index` past the last fold reads 0 with no
error.

## Full OHLCV path for `on_bar` strategies

`run_csv` replays close prices as synthetic trade events — `on_trade`
fires, but `on_bar` does not, and intrabar high / low / volume are
not preserved. For strategies whose decisions depend on bar internals
(TP/SL ladders on high/low, ATR-style indicators, breakout filters),
use `run_bars` with numpy arrays:

```python
import lrvx
import numpy as np

reg = lrvx.SymbolRegistry()
btc = reg.add_symbol("exchange", "BTCUSDT", 0.01)


class IntrabarBreakout(lrvx.Strategy):
    def __init__(self, syms):
        super().__init__(syms)
        self.in_pos = False

    def on_bar(self, ctx, bar):
        if not self.in_pos and bar.high >= bar.open * 1.005:
            self.market_buy(0.01)
            self.in_pos = True
        elif self.in_pos and bar.low <= bar.open * 0.99:
            self.market_sell(0.01)
            self.in_pos = False


wfr = lrvx.WalkForwardRunner(
    reg, fee_rate=0.0, initial_capital=10_000,
    mode="sliding", train_size=4380, test_size=2190, step=2190,
)
wfr.set_strategy_factory(lambda _i: IntrabarBreakout([btc]))

# OHLCV arrays — all must have the same length, sorted by end_time_ns.
start_ns = ...  # int64 ns, bar open
end_ns = ...    # int64 ns, bar close
open_, high, low, close, volume = ...  # float64

folds = wfr.run_bars(
    start_ns, end_ns, open_, high, low, close, volume,
    symbol="BTCUSDT")
```

Each fold dispatches `BarEvent`s with full OHLCV preserved. `on_bar`
fires; `on_trade` does not — same convention as
`BacktestRunner.run_bars`. `bar_type` (default `0` = Time) and
`bar_type_param` are forwarded for non-time bar aggregations.

## Optimising per fold (C++)

Give the C++ runner a parameter grid and a factory that takes parameters, and
each fold becomes a walk-forward optimisation: every grid point is run on the
train slice, the points are ranked, and the test slice runs on the winner.

```cpp
WalkForwardRunner runner(backtestConfig, wfConfig);

// One axis of candidate values per parameter; the runner walks the
// cartesian product, last axis varying fastest.
runner.setParameterGrid({{10.0, 20.0, 50.0}, {1.5, 2.0}});

runner.setStrategyFactory(
    [&](std::size_t foldIndex, const std::vector<double>& params) -> IStrategy*
    {
      // params[0] from the first axis, params[1] from the second.
      return makeStrategy(params[0], static_cast<int>(params[1]));
    });

const auto folds = runner.run(bars);
// folds[i].trainStats is the winning point's in-sample result;
// folds[i].testStats is that same point, out of sample.
```

Points are ranked on `net_pnl` — what the window kept after fees — and a tie
goes to the earlier grid point, so the selection is deterministic. The
factory is called once per grid point on the train slice and once more with
the winning point, so the out-of-sample run starts from clean strategy state.
Returning `nullptr` skips a point; a fold whose every grid point the
factory declines is reported empty — zeroed train and test stats and a
warning naming the fold — and the folds after it still run.

A factory that takes only a fold index keeps the older behaviour: one
strategy per window, no search. Setting a grid alongside that shape is
reported in the log and ignored — nothing could carry a parameter into the
strategy.

## What walk-forward does not do

The per-fold optimisation above is C++ only. The Python and Node runners
still take a fold-index factory and run one strategy per window; to optimise
from a binding, run [grid search](grid-search.md) on each fold's train slice
yourself and evaluate the winner on test.

The runner also has no pluggable objective: the in-sample ranking is
`net_pnl`. Rank on Sharpe or drawdown by composing the grid-search primitives
instead.
