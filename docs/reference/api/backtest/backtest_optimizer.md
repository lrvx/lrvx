# BacktestOptimizer

Grid search optimizer for strategy parameter tuning.

## Overview

`BacktestOptimizer` runs exhaustive parameter search by executing backtests for all combinations in a parameter grid and collecting results for analysis.

## Header

```cpp
#include "lrvx/backtest/backtest_optimizer.h"
```

## Template Parameters

```cpp
template <typename ParamsT, typename GridT>
class BacktestOptimizer;
```

- `ParamsT` - Parameter struct, must have `toString()` method
- `GridT` - Grid type, must implement `totalCombinations()` and `operator[]`

## Class Definition

```cpp
template <typename ParamsT, typename GridT>
class BacktestOptimizer {
public:
  using BacktestFactory = MoveOnlyFunction<BacktestResult(const ParamsT&)>;
  using ProgressCallback = MoveOnlyFunction<void(size_t, size_t, const OptimizationResult<ParamsT>&)>;

  BacktestOptimizer() = default;

  void setParameterGrid(const GridT& grid);
  void setBacktestFactory(BacktestFactory factory);
  void setProgressCallback(ProgressCallback callback);

  size_t totalCombinations() const;

  std::vector<OptimizationResult<ParamsT>> runLocal(size_t numThreads = 0);
  OptimizationResult<ParamsT> runSingle(size_t index);

  static std::vector<OptimizationResult<ParamsT>> rankResults(
      std::vector<OptimizationResult<ParamsT>> results,
      RankMetric metric,
      bool ascending = false);

  template <typename Predicate>
  static std::vector<OptimizationResult<ParamsT>> filterResults(
      const std::vector<OptimizationResult<ParamsT>>& results,
      Predicate&& predicate);

  static bool exportToCSV(
      const std::vector<OptimizationResult<ParamsT>>& results,
      const std::filesystem::path& path);
};
```

## Grid Requirements

```cpp
struct MyGrid {
  size_t totalCombinations() const;        // Total param combinations
  MyParams operator[](size_t index) const; // Get params at index
};
```

## Params Requirements

```cpp
struct MyParams {
  std::string toString() const;  // For CSV export
};
```

## Methods

### setParameterGrid

```cpp
void setParameterGrid(const GridT& grid);
```

Set the parameter grid to search.

### setBacktestFactory

```cpp
void setBacktestFactory(BacktestFactory factory);
```

Set function that creates and runs a backtest for given parameters.

### setProgressCallback

```cpp
void setProgressCallback(ProgressCallback callback);
```

Optional callback invoked periodically during optimization.

### runLocal

```cpp
std::vector<OptimizationResult<ParamsT>> runLocal(size_t numThreads = 0);
```

Runs every combination and returns one result per combination, in grid order; empty if no factory is set or the grid is empty. `numThreads` is the number of worker threads, 0 meaning one per hardware thread, and the calling thread is one of them. Combinations are handed out through an atomic index, so with more than one thread the factory is called from several threads at once: it has to build everything a backtest touches per call and share only read-only inputs. The progress callback is serialised and sees every tenth completion and the last, in completion order. An exception from the factory on any thread stops the hand-out and is rethrown once the workers have joined.

### runSingle

```cpp
OptimizationResult<ParamsT> runSingle(size_t index);
```

Run single backtest at grid index.

### rankResults (static)

```cpp
static std::vector<OptimizationResult<ParamsT>> rankResults(
    std::vector<OptimizationResult<ParamsT>> results,
    RankMetric metric,
    bool ascending = false);
```

Sort results by metric. Descending by default (best first).

### filterResults (static)

```cpp
template <typename Predicate>
static std::vector<OptimizationResult<ParamsT>> filterResults(
    const std::vector<OptimizationResult<ParamsT>>& results,
    Predicate&& predicate);
```

Filter results matching predicate.

### exportToCSV (static)

```cpp
static bool exportToCSV(
    const std::vector<OptimizationResult<ParamsT>>& results,
    const std::filesystem::path& path);
```

Export results to CSV file. Returns false on error.

## RankMetric

```cpp
enum class RankMetric {
  SharpeRatio,
  SortinoRatio,
  CalmarRatio,
  TotalReturn,
  MaxDrawdown,
  WinRate,
  ProfitFactor
};
```

## OptimizationResult

```cpp
template <typename ParamsT>
struct OptimizationResult {
  ParamsT parameters;
  BacktestStats stats;

  double sharpeRatio() const;
  double sortinoRatio() const;
  double calmarRatio() const;
  double totalReturn() const;
  double maxDrawdown() const;
  double maxDrawdownPct() const;
  double winRate() const;
  double profitFactor() const;
  size_t totalTrades() const;

  void setFromStats(const BacktestStats& s);
};
```

## Example

```cpp
struct MAParams {
  int fast, slow;
  std::string toString() const {
    return std::to_string(fast) + "," + std::to_string(slow);
  }
};

struct MAGrid {
  std::vector<int> fasts = {5, 10, 15};
  std::vector<int> slows = {20, 30, 40};
  size_t totalCombinations() const { return fasts.size() * slows.size(); }
  MAParams operator[](size_t i) const {
    return {fasts[i / slows.size()], slows[i % slows.size()]};
  }
};

BacktestOptimizer<MAParams, MAGrid> opt;
opt.setParameterGrid(MAGrid{});
opt.setBacktestFactory([](const MAParams& p) {
  return runMyStrategy(p);
});

auto results = opt.runLocal();
auto ranked = decltype(opt)::rankResults(results, RankMetric::SharpeRatio);
decltype(opt)::exportToCSV(ranked, "results.csv");
```

## See Also

- [optimization_stats.h](./optimization_stats.md)
- [BacktestResult](./backtest_result.md)
- [How-to: Grid Search](../../../how-to/grid-search.md)
