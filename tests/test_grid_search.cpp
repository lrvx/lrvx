/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/backtest/backtest_optimizer.h"
#include "lrvx/backtest/bar_replay_source.h"
#include "lrvx/backtest/grid_search.h"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

using namespace lrvx;

// Test parameter struct
struct TestParams
{
  int param1{0};
  int param2{0};

  std::string toString() const
  {
    return "p1=" + std::to_string(param1) + ",p2=" + std::to_string(param2);
  }
};

// Test grid that generates cartesian product
struct TestGrid
{
  std::vector<int> param1Values;
  std::vector<int> param2Values;

  size_t totalCombinations() const { return param1Values.size() * param2Values.size(); }

  TestParams operator[](size_t index) const
  {
    TestParams p;
    size_t i1 = index / param2Values.size();
    size_t i2 = index % param2Values.size();
    p.param1 = param1Values[i1];
    p.param2 = param2Values[i2];
    return p;
  }
};

class GridSearchTest : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    _test_dir = std::filesystem::temp_directory_path() / "lrvx_grid_search_test";
    std::filesystem::remove_all(_test_dir);
    std::filesystem::create_directories(_test_dir);
  }

  void TearDown() override { std::filesystem::remove_all(_test_dir); }

  std::filesystem::path _test_dir;
};

// BacktestOptimizer tests
TEST_F(GridSearchTest, OptimizerNoFactory)
{
  BacktestOptimizer<TestParams, TestGrid> optimizer;

  TestGrid grid;
  grid.param1Values = {1, 2};
  grid.param2Values = {10, 20};
  optimizer.setParameterGrid(grid);

  // No factory set - should return empty
  auto results = optimizer.runLocal();
  EXPECT_TRUE(results.empty());
}

TEST_F(GridSearchTest, OptimizerEmptyGrid)
{
  BacktestOptimizer<TestParams, TestGrid> optimizer;

  TestGrid grid;  // Empty grid
  optimizer.setParameterGrid(grid);

  optimizer.setBacktestFactory(
      [](const TestParams&)
      {
        BacktestConfig config;
        return BacktestResult(config);
      });

  auto results = optimizer.runLocal();
  EXPECT_TRUE(results.empty());
}

TEST_F(GridSearchTest, OptimizerBasicRun)
{
  BacktestOptimizer<TestParams, TestGrid> optimizer;

  TestGrid grid;
  grid.param1Values = {1, 2, 3};
  grid.param2Values = {10, 20};
  optimizer.setParameterGrid(grid);

  EXPECT_EQ(optimizer.totalCombinations(), 6);

  // runLocal() calls the factory from several threads at once, so the
  // counter it shares with them is atomic.
  std::atomic<int> callCount{0};
  optimizer.setBacktestFactory(
      [&callCount](const TestParams& params)
      {
        callCount++;
        BacktestConfig config;
        BacktestResult result(config);

        // Simulate some PnL based on params
        Fill buyFill;
        buyFill.orderId = 1;
        buyFill.symbol = 1;
        buyFill.side = Side::BUY;
        buyFill.price = Price::fromDouble(100.0);
        buyFill.quantity = Quantity::fromDouble(1.0);
        buyFill.timestampNs = UnixNanos::fromRaw(1000);
        result.recordFill(buyFill);

        Fill sellFill;
        sellFill.orderId = 2;
        sellFill.symbol = 1;
        sellFill.side = Side::SELL;
        sellFill.price = Price::fromDouble(100.0 + params.param1 + params.param2 * 0.1);
        sellFill.quantity = Quantity::fromDouble(1.0);
        sellFill.timestampNs = UnixNanos::fromRaw(2000);
        result.recordFill(sellFill);

        return result;
      });

  auto results = optimizer.runLocal();

  EXPECT_EQ(callCount.load(), 6);
  EXPECT_EQ(results.size(), 6);

  // Check that parameters are correctly assigned
  bool foundP1_2_P2_10 = false;
  for (const auto& r : results)
  {
    if (r.parameters.param1 == 2 && r.parameters.param2 == 10)
    {
      foundP1_2_P2_10 = true;
      EXPECT_GT(r.totalReturn(), 0);
    }
  }
  EXPECT_TRUE(foundP1_2_P2_10);
}

TEST_F(GridSearchTest, OptimizerProgressCallback)
{
  BacktestOptimizer<TestParams, TestGrid> optimizer;

  TestGrid grid;
  grid.param1Values = {1, 2, 3, 4, 5};
  grid.param2Values = {10, 20};
  optimizer.setParameterGrid(grid);

  optimizer.setBacktestFactory(
      [](const TestParams&)
      {
        BacktestConfig config;
        return BacktestResult(config);
      });

  std::vector<size_t> progressUpdates;
  optimizer.setProgressCallback(
      [&progressUpdates](size_t completed, size_t total, const OptimizationResult<TestParams>&)
      {
        progressUpdates.push_back(completed);
        EXPECT_EQ(total, 10);
      });

  optimizer.runLocal();

  // Progress is called every 10 completions or at the end
  EXPECT_FALSE(progressUpdates.empty());
  EXPECT_EQ(progressUpdates.back(), 10);  // Final update
}

TEST_F(GridSearchTest, OptimizerRunSingle)
{
  BacktestOptimizer<TestParams, TestGrid> optimizer;

  TestGrid grid;
  grid.param1Values = {1, 2, 3};
  grid.param2Values = {10, 20};
  optimizer.setParameterGrid(grid);

  optimizer.setBacktestFactory(
      [](const TestParams& params)
      {
        BacktestConfig config;
        BacktestResult result(config);

        Fill buyFill;
        buyFill.orderId = 1;
        buyFill.symbol = 1;
        buyFill.side = Side::BUY;
        buyFill.price = Price::fromDouble(100.0);
        buyFill.quantity = Quantity::fromDouble(1.0);
        buyFill.timestampNs = UnixNanos::fromRaw(1000);
        result.recordFill(buyFill);

        Fill sellFill;
        sellFill.orderId = 2;
        sellFill.symbol = 1;
        sellFill.side = Side::SELL;
        sellFill.price = Price::fromDouble(100.0 + params.param1);
        sellFill.quantity = Quantity::fromDouble(1.0);
        sellFill.timestampNs = UnixNanos::fromRaw(2000);
        result.recordFill(sellFill);

        return result;
      });

  // Run single backtest at index 0 (param1=1, param2=10)
  auto result = optimizer.runSingle(0);
  EXPECT_EQ(result.parameters.param1, 1);
  EXPECT_EQ(result.parameters.param2, 10);
}

TEST_F(GridSearchTest, RankResultsBySharpe)
{
  std::vector<OptimizationResult<TestParams>> results;

  for (int i = 0; i < 5; ++i)
  {
    OptimizationResult<TestParams> r;
    r.parameters.param1 = i;
    r.stats.sharpeRatio = static_cast<double>(i);
    results.push_back(r);
  }

  auto ranked = BacktestOptimizer<TestParams, TestGrid>::rankResults(results, RankMetric::SharpeRatio);

  // Descending order by default
  EXPECT_EQ(ranked[0].parameters.param1, 4);
  EXPECT_EQ(ranked[1].parameters.param1, 3);
  EXPECT_EQ(ranked[4].parameters.param1, 0);
}

TEST_F(GridSearchTest, RankResultsAscending)
{
  std::vector<OptimizationResult<TestParams>> results;

  for (int i = 0; i < 5; ++i)
  {
    OptimizationResult<TestParams> r;
    r.parameters.param1 = i;
    r.stats.maxDrawdown = static_cast<double>(i) * 10.0;
    results.push_back(r);
  }

  auto ranked = BacktestOptimizer<TestParams, TestGrid>::rankResults(
      results, RankMetric::MaxDrawdown, true);  // ascending = true

  // Ascending order
  EXPECT_DOUBLE_EQ(ranked[0].stats.maxDrawdown, 0.0);
  EXPECT_DOUBLE_EQ(ranked[4].stats.maxDrawdown, 40.0);
}

TEST_F(GridSearchTest, FilterResults)
{
  std::vector<OptimizationResult<TestParams>> results;

  for (int i = 0; i < 10; ++i)
  {
    OptimizationResult<TestParams> r;
    r.parameters.param1 = i;
    r.stats.sharpeRatio = static_cast<double>(i) * 0.5;
    r.stats.winRate = 0.5 + i * 0.02;
    results.push_back(r);
  }

  // Filter: sharpe > 2.0
  auto filtered = BacktestOptimizer<TestParams, TestGrid>::filterResults(
      results, [](const OptimizationResult<TestParams>& r)
      { return r.sharpeRatio() > 2.0; });

  EXPECT_EQ(filtered.size(), 5);  // indices 5-9 have sharpe > 2.0

  for (const auto& r : filtered)
  {
    EXPECT_GT(r.sharpeRatio(), 2.0);
  }
}

TEST_F(GridSearchTest, ExportToCSV)
{
  std::vector<OptimizationResult<TestParams>> results;

  for (int i = 0; i < 3; ++i)
  {
    OptimizationResult<TestParams> r;
    r.parameters.param1 = i;
    r.parameters.param2 = i * 10;
    r.stats.sharpeRatio = 1.0 + i * 0.1;
    r.stats.sortinoRatio = 1.5 + i * 0.1;
    r.stats.calmarRatio = 2.0;
    r.stats.totalPnl = 1000.0 + i * 100;
    r.stats.maxDrawdown = 50.0;
    r.stats.winRate = 0.6;
    r.stats.totalTrades = 100;
    results.push_back(r);
  }

  auto csvPath = _test_dir / "results.csv";
  bool success = BacktestOptimizer<TestParams, TestGrid>::exportToCSV(results, csvPath);

  EXPECT_TRUE(success);
  EXPECT_TRUE(std::filesystem::exists(csvPath));

  // Read and verify content
  std::ifstream file(csvPath);
  std::string line;

  // Header
  std::getline(file, line);
  EXPECT_TRUE(line.find("sharpe_ratio") != std::string::npos);
  EXPECT_TRUE(line.find("parameters") != std::string::npos);

  // Data rows
  int rowCount = 0;
  while (std::getline(file, line))
  {
    rowCount++;
    EXPECT_TRUE(line.find("p1=") != std::string::npos);
  }
  EXPECT_EQ(rowCount, 3);
}

TEST_F(GridSearchTest, ExportToCSVInvalidPath)
{
  std::vector<OptimizationResult<TestParams>> results;
  OptimizationResult<TestParams> r;
  r.stats.sharpeRatio = 1.0;
  results.push_back(r);

  bool success =
      BacktestOptimizer<TestParams, TestGrid>::exportToCSV(results, "/nonexistent/path/file.csv");
  EXPECT_FALSE(success);
}

// OptimizationResult tests
TEST_F(GridSearchTest, OptimizationResultAccessors)
{
  OptimizationResult<TestParams> r;
  r.parameters.param1 = 5;
  r.stats.sharpeRatio = 1.5;
  r.stats.sortinoRatio = 2.0;
  r.stats.calmarRatio = 2.5;
  r.stats.totalPnl = 1000.0;
  r.stats.maxDrawdown = 100.0;
  r.stats.maxDrawdownPct = 10.0;
  r.stats.winRate = 0.6;
  r.stats.profitFactor = 2.0;
  r.stats.totalTrades = 50;

  EXPECT_DOUBLE_EQ(r.sharpeRatio(), 1.5);
  EXPECT_DOUBLE_EQ(r.sortinoRatio(), 2.0);
  EXPECT_DOUBLE_EQ(r.calmarRatio(), 2.5);
  EXPECT_DOUBLE_EQ(r.totalReturn(), 1000.0);
  EXPECT_DOUBLE_EQ(r.maxDrawdown(), 100.0);
  EXPECT_DOUBLE_EQ(r.maxDrawdownPct(), 10.0);
  EXPECT_DOUBLE_EQ(r.winRate(), 0.6);
  EXPECT_DOUBLE_EQ(r.profitFactor(), 2.0);
  EXPECT_EQ(r.totalTrades(), 50);
}

TEST_F(GridSearchTest, OptimizationResultSetFromStats)
{
  OptimizationResult<TestParams> r;

  BacktestStats stats;
  stats.sharpeRatio = 3.0;
  stats.totalTrades = 200;

  r.setFromStats(stats);

  EXPECT_DOUBLE_EQ(r.sharpeRatio(), 3.0);
  EXPECT_EQ(r.totalTrades(), 200);
}

// Grid tests
TEST_F(GridSearchTest, GridTotalCombinations)
{
  TestGrid grid;
  grid.param1Values = {1, 2, 3};
  grid.param2Values = {10, 20, 30, 40};

  EXPECT_EQ(grid.totalCombinations(), 12);
}

TEST_F(GridSearchTest, GridIndexOperator)
{
  TestGrid grid;
  grid.param1Values = {1, 2};
  grid.param2Values = {10, 20, 30};

  // Index 0: param1=1, param2=10
  auto p0 = grid[0];
  EXPECT_EQ(p0.param1, 1);
  EXPECT_EQ(p0.param2, 10);

  // Index 1: param1=1, param2=20
  auto p1 = grid[1];
  EXPECT_EQ(p1.param1, 1);
  EXPECT_EQ(p1.param2, 20);

  // Index 3: param1=2, param2=10
  auto p3 = grid[3];
  EXPECT_EQ(p3.param1, 2);
  EXPECT_EQ(p3.param2, 10);

  // Index 5: param1=2, param2=30
  auto p5 = grid[5];
  EXPECT_EQ(p5.param1, 2);
  EXPECT_EQ(p5.param2, 30);
}

TEST_F(GridSearchTest, RankByAllMetrics)
{
  std::vector<OptimizationResult<TestParams>> results;

  for (int i = 0; i < 5; ++i)
  {
    OptimizationResult<TestParams> r;
    r.stats.sharpeRatio = static_cast<double>(i);
    r.stats.sortinoRatio = static_cast<double>(i);
    r.stats.calmarRatio = static_cast<double>(i);
    r.stats.totalPnl = static_cast<double>(i) * 100;
    r.stats.maxDrawdown = static_cast<double>(i) * 10;
    r.stats.winRate = static_cast<double>(i) * 0.1;
    r.stats.profitFactor = static_cast<double>(i) * 0.5;
    results.push_back(r);
  }

  auto r1 = BacktestOptimizer<TestParams, TestGrid>::rankResults(results, RankMetric::SharpeRatio);
  EXPECT_DOUBLE_EQ(r1[0].sharpeRatio(), 4.0);

  auto r2 = BacktestOptimizer<TestParams, TestGrid>::rankResults(results, RankMetric::SortinoRatio);
  EXPECT_DOUBLE_EQ(r2[0].sortinoRatio(), 4.0);

  auto r3 = BacktestOptimizer<TestParams, TestGrid>::rankResults(results, RankMetric::CalmarRatio);
  EXPECT_DOUBLE_EQ(r3[0].calmarRatio(), 4.0);

  auto r4 = BacktestOptimizer<TestParams, TestGrid>::rankResults(results, RankMetric::TotalReturn);
  EXPECT_DOUBLE_EQ(r4[0].totalReturn(), 400.0);

  auto r5 = BacktestOptimizer<TestParams, TestGrid>::rankResults(results, RankMetric::MaxDrawdown);
  EXPECT_DOUBLE_EQ(r5[0].maxDrawdown(), 40.0);

  auto r6 = BacktestOptimizer<TestParams, TestGrid>::rankResults(results, RankMetric::WinRate);
  EXPECT_DOUBLE_EQ(r6[0].winRate(), 0.4);

  auto r7 = BacktestOptimizer<TestParams, TestGrid>::rankResults(results, RankMetric::ProfitFactor);
  EXPECT_DOUBLE_EQ(r7[0].profitFactor(), 2.0);
}

// ---------------------------------------------------------------- threads

namespace
{

// A result that depends on nothing but its parameters, so a run on any
// thread count must come back identical.
BacktestResult pnlFor(int p1, int p2)
{
  BacktestConfig config;
  BacktestResult result(config);

  Fill buy;
  buy.orderId = 1;
  buy.symbol = 1;
  buy.side = Side::BUY;
  buy.price = Price::fromDouble(100.0);
  buy.quantity = Quantity::fromDouble(1.0);
  buy.timestampNs = UnixNanos::fromRaw(1000);
  result.recordFill(buy);

  Fill sell = buy;
  sell.orderId = 2;
  sell.side = Side::SELL;
  sell.price = Price::fromDouble(100.0 + p1 + p2 * 0.1);
  sell.timestampNs = UnixNanos::fromRaw(2000);
  result.recordFill(sell);
  return result;
}

TestGrid sevenByThree()
{
  TestGrid grid;
  grid.param1Values = {1, 2, 3, 4, 5, 6, 7};
  grid.param2Values = {10, 20, 30};
  return grid;
}

}  // namespace

TEST_F(GridSearchTest, OptimizerThreadsAgreeWithTheSequentialRun)
{
  auto run = [](size_t threads, std::atomic<int>& calls)
  {
    BacktestOptimizer<TestParams, TestGrid> optimizer;
    optimizer.setParameterGrid(sevenByThree());
    optimizer.setBacktestFactory(
        [&calls](const TestParams& p)
        {
          ++calls;
          return pnlFor(p.param1, p.param2);
        });
    return optimizer.runLocal(threads);
  };

  std::atomic<int> seqCalls{0};
  std::atomic<int> parCalls{0};
  auto seq = run(1, seqCalls);
  auto par = run(4, parCalls);

  ASSERT_EQ(seq.size(), 21u);
  ASSERT_EQ(par.size(), 21u);
  EXPECT_EQ(seqCalls.load(), 21);
  EXPECT_EQ(parCalls.load(), 21);
  for (size_t i = 0; i < seq.size(); ++i)
  {
    EXPECT_EQ(par[i].parameters.param1, seq[i].parameters.param1) << "combination " << i;
    EXPECT_EQ(par[i].parameters.param2, seq[i].parameters.param2) << "combination " << i;
    EXPECT_DOUBLE_EQ(par[i].totalReturn(), seq[i].totalReturn()) << "combination " << i;
    EXPECT_EQ(par[i].totalTrades(), seq[i].totalTrades()) << "combination " << i;
  }
}

TEST_F(GridSearchTest, OptimizerRunsTheFactoryOnMoreThanOneThread)
{
  BacktestOptimizer<TestParams, TestGrid> optimizer;
  TestGrid grid;
  grid.param1Values = {1, 2, 3, 4};
  grid.param2Values = {10, 20, 30, 40};
  optimizer.setParameterGrid(grid);

  std::mutex mutex;
  std::set<std::thread::id> threads;
  optimizer.setBacktestFactory(
      [&](const TestParams& p)
      {
        {
          std::lock_guard<std::mutex> lock(mutex);
          threads.insert(std::this_thread::get_id());
        }
        // Long enough that the other workers pick up combinations of
        // their own while this one is busy.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return pnlFor(p.param1, p.param2);
      });

  auto results = optimizer.runLocal(4);
  EXPECT_EQ(results.size(), 16u);
  EXPECT_GE(threads.size(), 2u) << "every combination ran on the calling thread";
}

TEST_F(GridSearchTest, OptimizerProgressCallbackIsSerialisedAcrossThreads)
{
  BacktestOptimizer<TestParams, TestGrid> optimizer;
  TestGrid grid;
  grid.param1Values = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  grid.param2Values = {10, 20, 30, 40};
  optimizer.setParameterGrid(grid);
  optimizer.setBacktestFactory(
      [](const TestParams& p)
      {
        return pnlFor(p.param1, p.param2);
      });

  std::atomic<int> inside{0};
  std::atomic<int> overlaps{0};
  std::mutex mutex;
  std::set<size_t> completions;
  optimizer.setProgressCallback(
      [&](size_t completed, size_t total, const OptimizationResult<TestParams>&)
      {
        if (++inside != 1)
        {
          ++overlaps;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        EXPECT_EQ(total, 40u);
        {
          std::lock_guard<std::mutex> lock(mutex);
          completions.insert(completed);
        }
        --inside;
      });

  auto results = optimizer.runLocal(4);
  EXPECT_EQ(results.size(), 40u);
  EXPECT_EQ(overlaps.load(), 0);
  EXPECT_EQ(completions, (std::set<size_t>{10, 20, 30, 40}));
}

TEST_F(GridSearchTest, OptimizerFactoryExceptionIsRethrownFromEveryThreadCount)
{
  for (size_t threads : {size_t{1}, size_t{4}})
  {
    BacktestOptimizer<TestParams, TestGrid> optimizer;
    optimizer.setParameterGrid(sevenByThree());
    optimizer.setBacktestFactory(
        [](const TestParams& p)
        {
          if (p.param1 == 5)
          {
            throw std::runtime_error("combination refused");
          }
          return pnlFor(p.param1, p.param2);
        });
    EXPECT_THROW(optimizer.runLocal(threads), std::runtime_error) << "threads=" << threads;
  }
}

TEST(GridSearchTypeErased, ThreadsAgreeWithTheSequentialRun)
{
  auto run = [](std::size_t threads)
  {
    GridSearch gs;
    gs.addAxis({1.0, 2.0, 3.0, 4.0, 5.0});
    gs.addAxis({10.0, 20.0, 30.0});
    gs.setFactory(
        [](const std::vector<double>& params)
        {
          return pnlFor(static_cast<int>(params[0]), static_cast<int>(params[1]));
        });
    return gs.run(threads);
  };

  auto seq = run(1);
  auto par = run(4);
  ASSERT_EQ(seq.size(), 15u);
  ASSERT_EQ(par.size(), 15u);
  for (std::size_t i = 0; i < seq.size(); ++i)
  {
    EXPECT_EQ(par[i].paramIndex, i);
    EXPECT_EQ(par[i].params, seq[i].params) << "combination " << i;
    EXPECT_DOUBLE_EQ(par[i].stats.totalPnl, seq[i].stats.totalPnl) << "combination " << i;
    EXPECT_EQ(par[i].stats.totalTrades, seq[i].stats.totalTrades) << "combination " << i;
  }
}

TEST(GridSearchTypeErased, FactoryExceptionIsRethrown)
{
  GridSearch gs;
  gs.addAxis({1.0, 2.0, 3.0});
  gs.addAxis({10.0, 20.0});
  gs.setFactory(
      [](const std::vector<double>& params)
      {
        if (params[0] == 2.0)
        {
          throw std::runtime_error("combination refused");
        }
        return pnlFor(static_cast<int>(params[0]), static_cast<int>(params[1]));
      });
  EXPECT_THROW(gs.run(4), std::runtime_error);
}
