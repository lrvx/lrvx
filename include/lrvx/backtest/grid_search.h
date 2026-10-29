/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/backtest/backtest_result.h"
#include "lrvx/util/base/move_only_function.h"

#include <cstddef>
#include <vector>

namespace lrvx
{

struct GridSearchResult
{
  std::size_t paramIndex{};
  std::vector<double> params;
  BacktestStats stats;
};

/// Type-erased grid search over numeric parameter axes.
///
/// Unlike `BacktestOptimizer<ParamsT, GridT>` (template-based, C++
/// only), this version stores axes as `vector<vector<double>>` so it
/// can cross language boundaries through the C ABI. The user-supplied
/// factory builds a `BacktestResult` from a plain `vector<double>`
/// of parameter values — one slot per axis, in the same order as
/// `addAxis` calls.
class GridSearch
{
 public:
  /// Build a BacktestResult from one parameter point.
  using BacktestFactory =
      MoveOnlyFunction<BacktestResult(const std::vector<double>& params)>;

  GridSearch() = default;

  /// Append an axis of values. Each call adds one parameter dimension.
  void addAxis(std::vector<double> values);

  void setFactory(BacktestFactory factory);

  /// Total number of parameter combinations (product of axis lengths).
  std::size_t totalCombinations() const;

  /// Resolve a flat index back into a vector of parameter values, one
  /// entry per axis in registration order.
  std::vector<double> paramsForIndex(std::size_t index) const;

  /// Run every combination and return one result per combination, in
  /// flat-index order. `numThreads` is the number of worker threads, 0
  /// meaning one per hardware thread; the calling thread is one of them.
  /// With more than one thread the factory is called from several threads
  /// at once, so it has to build everything a backtest touches per call and
  /// share only read-only inputs. An exception from the factory on any
  /// thread stops the hand-out and is rethrown once the workers have joined.
  /// The language bindings do not go through this method: their factories
  /// run on the binding's own thread (the Python factory holds the GIL, a
  /// Node factory is a JavaScript function), so `GridSearch.run()` there is
  /// sequential and a parallel sweep uses processes.
  std::vector<GridSearchResult> run(std::size_t numThreads = 0);

 private:
  std::vector<std::vector<double>> _axes;
  BacktestFactory _factory;
};

}  // namespace lrvx
