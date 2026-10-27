/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "lrvx/backtest/backtest_runner.h"
#include "lrvx/log/log.h"
#include "lrvx/util/base/move_only_function.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

namespace lrvx
{

enum class RankMetric
{
  SharpeRatio,
  SortinoRatio,
  CalmarRatio,
  TotalReturn,
  MaxDrawdown,
  WinRate,
  ProfitFactor
};

template <typename ParamsT>
struct OptimizationResult
{
  ParamsT parameters;
  BacktestStats stats;

  double sharpeRatio() const { return stats.sharpeRatio; }
  double sortinoRatio() const { return stats.sortinoRatio; }
  double calmarRatio() const { return stats.calmarRatio; }
  double totalReturn() const { return stats.totalPnl; }
  double maxDrawdown() const { return stats.maxDrawdown; }
  // Already a percent: BacktestResult scales the drawdown fraction by 100
  // when it builds the equity curve. Print it straight into a "%" -- the
  // report used to scale it a second time and published 10% as 1000.00%.
  double maxDrawdownPct() const { return stats.maxDrawdownPct; }
  // A fraction, unlike maxDrawdownPct: 0.5 is a 50% win rate, so a display
  // that wants a percent does have to scale this one.
  double winRate() const { return stats.winRate; }
  double profitFactor() const { return stats.profitFactor; }
  size_t totalTrades() const { return stats.totalTrades; }

  void setFromStats(const BacktestStats& s) { stats = s; }
};

template <typename ParamsT, typename GridT>
class BacktestOptimizer
{
 public:
  using BacktestFactory = MoveOnlyFunction<BacktestResult(const ParamsT&)>;
  using ProgressCallback = MoveOnlyFunction<void(size_t completed, size_t total, const OptimizationResult<ParamsT>& latest)>;

  BacktestOptimizer() = default;

  void setParameterGrid(const GridT& grid) { _grid = grid; }
  void setBacktestFactory(BacktestFactory factory) { _factory = std::move(factory); }
  void setProgressCallback(ProgressCallback callback) { _progressCallback = std::move(callback); }

  size_t totalCombinations() const { return _grid.totalCombinations(); }

  // Runs every combination and returns one result per combination, in grid
  // order. numThreads is the number of worker threads, 0 meaning one per
  // hardware thread; the calling thread is one of them. Combinations are
  // handed out through an atomic index, so with more than one thread the
  // factory is called from several threads at once: it has to build
  // everything a backtest touches per call and share only read-only inputs
  // (a bar vector loaded once is the usual one). The progress callback is
  // serialised and sees every tenth completion and the last, in completion
  // order rather than grid order. An exception from the factory on any
  // thread stops the hand-out, and is rethrown here once the workers have
  // joined.
  std::vector<OptimizationResult<ParamsT>> runLocal(size_t numThreads = 0)
  {
    if (!_factory)
    {
      LRVX_LOG_ERROR("BacktestOptimizer: No factory set");
      return {};
    }

    const size_t total = _grid.totalCombinations();
    if (total == 0)
    {
      LRVX_LOG_ERROR("BacktestOptimizer: No parameter combinations");
      return {};
    }

    const size_t threads = resolveThreads(numThreads, total);
    LRVX_LOG_INFO("Running optimization: " << total << " parameter combinations on " << threads
                                           << " thread(s)");

    std::vector<OptimizationResult<ParamsT>> results(total);
    _completedTasks = 0;

    std::atomic<size_t> next{0};
    std::mutex progressMutex;
    std::mutex failureMutex;
    std::exception_ptr failure;

    auto worker = [&]
    {
      for (;;)
      {
        const size_t i = next.fetch_add(1, std::memory_order_relaxed);
        if (i >= total)
        {
          return;
        }
        try
        {
          ParamsT params = _grid[i];
          BacktestResult bt = _factory(params);
          auto stats = bt.computeStats();

          OptimizationResult<ParamsT>& res = results[i];
          res.parameters = params;
          res.setFromStats(stats);

          const size_t completed = ++_completedTasks;
          if (_progressCallback && (completed % 10 == 0 || completed == total))
          {
            std::lock_guard<std::mutex> lock(progressMutex);
            _progressCallback(completed, total, res);
          }
        }
        catch (...)
        {
          std::lock_guard<std::mutex> lock(failureMutex);
          if (!failure)
          {
            failure = std::current_exception();
          }
          // Nothing else starts; the combinations in flight finish on
          // their own threads.
          next.store(total, std::memory_order_relaxed);
          return;
        }
      }
    };

    std::vector<std::thread> pool;
    pool.reserve(threads - 1);
    for (size_t t = 1; t < threads; ++t)
    {
      pool.emplace_back(worker);
    }
    worker();
    for (auto& th : pool)
    {
      th.join();
    }

    if (failure)
    {
      std::rethrow_exception(failure);
    }

    LRVX_LOG_INFO("Optimization complete: " << total << " backtests finished");
    return results;
  }

  OptimizationResult<ParamsT> runSingle(size_t index)
  {
    ParamsT params = _grid[index];
    BacktestResult bt = _factory(params);
    auto stats = bt.computeStats();

    OptimizationResult<ParamsT> res;
    res.parameters = params;
    res.setFromStats(stats);
    return res;
  }

  static std::vector<OptimizationResult<ParamsT>> rankResults(
      std::vector<OptimizationResult<ParamsT>> results,
      RankMetric metric,
      bool ascending = false)
  {
    auto comparator = [metric, ascending](const OptimizationResult<ParamsT>& a, const OptimizationResult<ParamsT>& b)
    {
      double valA = 0.0, valB = 0.0;
      switch (metric)
      {
        case RankMetric::SharpeRatio:
          valA = a.sharpeRatio();
          valB = b.sharpeRatio();
          break;
        case RankMetric::SortinoRatio:
          valA = a.sortinoRatio();
          valB = b.sortinoRatio();
          break;
        case RankMetric::CalmarRatio:
          valA = a.calmarRatio();
          valB = b.calmarRatio();
          break;
        case RankMetric::TotalReturn:
          valA = a.totalReturn();
          valB = b.totalReturn();
          break;
        case RankMetric::MaxDrawdown:
          valA = a.maxDrawdown();
          valB = b.maxDrawdown();
          break;
        case RankMetric::WinRate:
          valA = a.winRate();
          valB = b.winRate();
          break;
        case RankMetric::ProfitFactor:
          valA = a.profitFactor();
          valB = b.profitFactor();
          break;
      }
      return ascending ? (valA < valB) : (valA > valB);
    };

    std::sort(results.begin(), results.end(), comparator);
    return results;
  }

  template <typename Predicate>
  static std::vector<OptimizationResult<ParamsT>> filterResults(
      const std::vector<OptimizationResult<ParamsT>>& results,
      Predicate&& predicate)
  {
    std::vector<OptimizationResult<ParamsT>> filtered;
    std::copy_if(results.begin(), results.end(), std::back_inserter(filtered),
                 std::forward<Predicate>(predicate));
    return filtered;
  }

  static bool exportToCSV(const std::vector<OptimizationResult<ParamsT>>& results,
                          const std::filesystem::path& path)
  {
    std::ofstream file(path);
    if (!file.is_open())
    {
      LRVX_LOG_ERROR("Failed to open CSV file: " << path.string());
      return false;
    }

    file << "sharpe_ratio,sortino_ratio,calmar_ratio,total_return,max_drawdown,win_rate,total_trades,parameters\n";
    for (const auto& res : results)
    {
      file << res.sharpeRatio() << ","
           << res.sortinoRatio() << ","
           << res.calmarRatio() << ","
           << res.totalReturn() << ","
           << res.maxDrawdown() << ","
           << res.winRate() << ","
           << res.totalTrades() << ","
           << res.parameters.toString() << "\n";
    }
    return true;
  }

 private:
  static size_t resolveThreads(size_t requested, size_t total)
  {
    size_t n = requested == 0 ? static_cast<size_t>(std::thread::hardware_concurrency()) : requested;
    if (n == 0)
    {
      n = 1;
    }
    return std::min(n, total);
  }

  GridT _grid;
  BacktestFactory _factory;
  ProgressCallback _progressCallback;
  std::atomic<size_t> _completedTasks{0};
};

}  // namespace lrvx
