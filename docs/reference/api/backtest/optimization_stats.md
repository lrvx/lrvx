# OptimizationStatistics

Statistical utilities for analyzing optimization results.

## Overview

`OptimizationStatistics` provides statistical functions for analyzing and comparing backtest results: confidence intervals, significance tests, correlations, and reporting.

## Header

```cpp
#include "lrvx/backtest/optimization_stats.h"
```

## Helper Functions (detail namespace)

```cpp
namespace detail {
  double mean(const std::vector<double>& data);
  double variance(const std::vector<double>& data);
  double stddev(const std::vector<double>& data);
}
```

Basic statistical functions. Returns 0.0 for empty data.

## extractMetric

```cpp
template <typename ParamsT>
std::vector<double> extractMetric(
    const std::vector<OptimizationResult<ParamsT>>& results,
    RankMetric metric);
```

Extract specified metric values from all results.

**Example:**
```cpp
auto sharpes = extractMetric(results, RankMetric::SharpeRatio);
double avgSharpe = detail::mean(sharpes);
```

## Class Definition

```cpp
template <typename ParamsT, typename GridT>
class OptimizationStatistics {
public:
  struct ConfidenceInterval {
    double lower;
    double median;
    double upper;
  };

  static double permutationTest(
      const std::vector<double>& group1,
      const std::vector<double>& group2,
      size_t numPermutations = 10000,
      std::uint64_t seed = 42u);

  static double correlation(
      const std::vector<double>& x,
      const std::vector<double>& y);

  static ConfidenceInterval bootstrapCI(
      const std::vector<double>& data,
      double confidenceLevel = 0.95,
      size_t numSamples = 10000,
      std::uint64_t seed = 42u);

  static void printSummary(
      const std::vector<OptimizationResult<ParamsT>>& results);

  static bool generateReport(
      const std::vector<OptimizationResult<ParamsT>>& results,
      const std::filesystem::path& outputPath);
};
```

## Methods

### permutationTest

```cpp
static double permutationTest(
    const std::vector<double>& group1,
    const std::vector<double>& group2,
    size_t numPermutations = 10000,
    std::uint64_t seed = 42u);
```

Two-sample permutation test for comparing group means. Returns a p-value,
always strictly greater than zero: the p-value is `(extreme + 1) /
(numPermutations + 1)`, the standard add-one correction (the observed
arrangement is itself one of the possible outcomes under the null).

The resample is seeded (default `42`, matching
[`whitesRealityCheck`](../../../how-to/whites-reality-check.md)), so the
same inputs always return the same p-value. Pass a different `seed` to draw
an independent resample.

**Example:**
```cpp
// Compare two parameter configurations
std::vector<double> configA = {1.2, 1.5, 1.3};
std::vector<double> configB = {0.8, 0.9, 0.7};
double pValue = Stats::permutationTest(configA, configB);
// pValue < 0.05 suggests significant difference
```

### correlation

```cpp
static double correlation(
    const std::vector<double>& x,
    const std::vector<double>& y);
```

Pearson correlation coefficient between two vectors. Returns value in [-1, 1].

**Example:**
```cpp
auto sharpes = extractMetric(results, RankMetric::SharpeRatio);
auto returns = extractMetric(results, RankMetric::TotalReturn);
double r = Stats::correlation(sharpes, returns);
```

### bootstrapCI

```cpp
static ConfidenceInterval bootstrapCI(
    const std::vector<double>& data,
    double confidenceLevel = 0.95,
    size_t numSamples = 10000,
    std::uint64_t seed = 42u);
```

Bootstrap confidence interval for the mean. `confidenceLevel` is clamped to
`[0, 1]`; `numSamples == 0` returns `{0.0, 0.0, 0.0}` instead of resampling.
Seeded the same way as `permutationTest` above: deterministic by default,
override `seed` for an independent resample.

**Example:**
```cpp
auto sharpes = extractMetric(results, RankMetric::SharpeRatio);
auto ci = Stats::bootstrapCI(sharpes, 0.95);
// ci.lower, ci.median, ci.upper
```

### printSummary

```cpp
static void printSummary(
    const std::vector<OptimizationResult<ParamsT>>& results);
```

Print optimization summary to log. Shows total combinations, mean/stddev Sharpe, and best result details.

### generateReport

```cpp
static bool generateReport(
    const std::vector<OptimizationResult<ParamsT>>& results,
    const std::filesystem::path& outputPath);
```

Generate a Markdown report with a top-10 results table and statistics.
Returns `true` on success; `false` (and an error logged) if `outputPath`
could not be opened for writing, e.g. a nonexistent parent directory.

## Example

```cpp
using Stats = OptimizationStatistics<MAParams, MAGrid>;

// Run optimization
auto results = optimizer.runLocal();

// Quick summary
Stats::printSummary(results);

// Full report
Stats::generateReport(results, "optimization_report.md");

// Statistical analysis
auto sharpes = extractMetric(results, RankMetric::SharpeRatio);
auto ci = Stats::bootstrapCI(sharpes);
std::cout << "95% CI: [" << ci.lower << ", " << ci.upper << "]\n";

// Compare top vs bottom half
auto ranked = optimizer.rankResults(results, RankMetric::SharpeRatio);
size_t mid = ranked.size() / 2;
std::vector<double> topHalf, bottomHalf;
for (size_t i = 0; i < mid; ++i) {
  topHalf.push_back(ranked[i].sharpeRatio());
  bottomHalf.push_back(ranked[mid + i].sharpeRatio());
}
double pValue = Stats::permutationTest(topHalf, bottomHalf);
```

## Robustness statistics (`lrvx/stats/`)

Header-only, outside `OptimizationStatistics`; each takes raw arrays so a binding can call it on its own buffers.

```cpp
#include "lrvx/stats/pbo_cscv.h"
#include "lrvx/stats/deflated_sharpe.h"
#include "lrvx/stats/grid_neighborhood.h"

// returns: trials x periods, row-major. pbo is NaN and splits 0 when the
// inputs cannot be split (fewer than 2 trials, S odd or above 32, T < S).
stats::PboResult pbo = stats::pboCscv(returns.data(), trials, periods, /*numBlocks=*/16);

// The selected strategy's returns and every trial's per-period Sharpe ratio.
stats::DeflatedSharpeResult d =
    stats::deflatedSharpeRatio(best.data(), best.size(), sharpes.data(), sharpes.size());

// values row-major over shape; one mean and one count per cell.
stats::gridNeighborhood(values.data(), shape, ndim, /*radius=*/1, means.data(), counts.data());
```

`PboResult` carries `pbo`, `splits`, `meanOosRank` and `meanLogit`; `DeflatedSharpeResult` carries `sharpe`, `skewness`, `kurtosis`, `expectedMaxSharpe` and `dsr`. The definitions, with references, are on [Overfitting checks for a parameter sweep](../../../how-to/robustness-statistics.md); `lrvx/stats/normal.h` holds the normal CDF and quantile they use.

## See Also

- [BacktestOptimizer](./backtest_optimizer.md)
- [How-to: Grid Search](../../../how-to/grid-search.md)
