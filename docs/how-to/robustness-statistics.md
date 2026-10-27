# Overfitting checks for a parameter sweep

A sweep over N parameter combinations always has a winner. These three statistics say what that winner is worth: whether the selection itself overfits, whether the winning Sharpe ratio clears the bar that N unskilled trials set, and whether the winning point sits on a plateau or on a spike. They work on the sweep's own outputs, the per-period returns and the metric per combination, and need nothing from the engine.

## Probability of backtest overfitting

`pbo_cscv(returns, num_blocks=16)` takes the per-period returns of every combination as a `(trials, periods)` array and runs combinatorially symmetric cross-validation: the periods are cut into `num_blocks` blocks, every way of choosing half of the blocks is an in-sample set with the other half out of sample, and on each split the in-sample best combination is ranked among all combinations out of sample. The relative rank is `r / (N + 1)`, its logit `log(w / (1 - w))`, and `pbo` is the fraction of splits whose logit is below zero: the in-sample winner did worse than the out-of-sample median. A sweep whose winner is a fit to the sample answers around 0.5 or more; a real edge answers near 0.

The result also carries `splits` (there are 12 870 with 16 blocks), `mean_oos_rank` and `mean_logit`. Nothing is sampled, so the same inputs give the same answer. The inputs cannot be split with fewer than two trials, an odd block count, or fewer periods than blocks, and `pbo` is then NaN.

Reference: Bailey, Borwein, López de Prado, Zhu (2017), "The Probability of Backtest Overfitting", Journal of Computational Finance 20(4).

## Deflated Sharpe ratio

`deflated_sharpe(returns, trial_sharpes=None)` takes the selected combination's per-period returns and the per-period Sharpe ratio of every trial the selection ran over. The expected maximum Sharpe ratio of that many unskilled trials, `expected_max_sharpe`, follows from the number of trials and the variance of their Sharpe ratios; `dsr` is the probability that the selected strategy's true Sharpe ratio is above it, with the sample length, the skewness and the kurtosis of the returns taken into account. Without `trial_sharpes` there is no selection, the expected maximum is 0, and `dsr` is the probabilistic Sharpe ratio against zero.

The function returns `sharpe` (per period, mean over sample standard deviation), `skewness`, `kurtosis` (3 for normal returns), `expected_max_sharpe` and `dsr`. Annualise nothing before calling it: every quantity is per period. Fewer than two periods, or a zero return variance, answer NaN throughout.

References: Bailey, López de Prado (2014), "The Deflated Sharpe Ratio", Journal of Portfolio Management 40(5); Bailey, López de Prado (2012), "The Sharpe Ratio Efficient Frontier", Journal of Risk 15(2).

## Neighbourhood of a grid point

`grid_neighborhood(values, radius=1)` takes the metric of every combination as an n-dimensional array over the parameter grid and returns, for every cell, `neighbor_mean` (the metric averaged over the cells whose index differs by at most `radius` on every axis, the cell itself excluded, NaN cells skipped) and `neighbor_count`. Both have the shape of `values`. A corner of a two-dimensional grid has three neighbours at radius 1, an interior point eight; a cell with none gets NaN.

The decision is the caller's. A winner whose neighbourhood mean is close to its own value sits on a plateau, and a small move in either parameter keeps most of the edge; a winner far above its neighbours is a spike, which is what a fit to noise looks like on a grid.

## The three together

```python
--8<-- "examples/python_robustness_statistics.py"
```

In C++ the same functions are `lrvx::stats::pboCscv`, `lrvx::stats::deflatedSharpeRatio` and `lrvx::stats::gridNeighborhood` in `lrvx/stats/`; in Node they are `pboCscv`, `deflatedSharpe` and `gridNeighborhood`.

## See also

- [Grid search](grid-search.md)
- [Sweep parameters over bars](sweep-over-bars.md)
- [Walk-forward](walk-forward.md)
- [White's reality check](whites-reality-check.md)
