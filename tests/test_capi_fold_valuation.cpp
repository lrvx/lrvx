/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// The open position at the end of each walk-forward fold and each grid-search
// combination, through the C API. LrvxBacktestStats and LrvxWalkForwardFold
// keep their shape and LRVX_CAPI_ABI_VERSION stays 6; the values come from
// additive functions.
//
// Walk-forward -- an owned result handle, run like lrvx_walk_forward_run_csv:
//
//   LrvxWalkForwardResultHandle lrvx_walk_forward_run_csv_result(
//       LrvxRegistryHandle registry, const char* csv_path, const char* symbol,
//       double fee_rate, double initial_capital, const LrvxWalkForwardConfig* cfg,
//       LrvxWalkForwardFactoryFn factory, void* user_data);
//   void     lrvx_walk_forward_result_destroy(LrvxWalkForwardResultHandle result);
//   uint32_t lrvx_walk_forward_result_folds(LrvxWalkForwardResultHandle result,
//                                           LrvxWalkForwardFold* folds_out, uint32_t max_folds);
//   double   lrvx_walk_forward_result_train_unrealized_pnl(LrvxWalkForwardResultHandle result,
//                                                          uint32_t fold_index);
//   double   lrvx_walk_forward_result_train_final_equity(LrvxWalkForwardResultHandle result,
//                                                        uint32_t fold_index);
//   double   lrvx_walk_forward_result_test_unrealized_pnl(LrvxWalkForwardResultHandle result,
//                                                         uint32_t fold_index);
//   double   lrvx_walk_forward_result_test_final_equity(LrvxWalkForwardResultHandle result,
//                                                       uint32_t fold_index);
//
// run_csv_result fails where lrvx_walk_forward_run_csv does (a CSV it cannot
// open or parse, a symbol the registry does not hold, a NULL registry, path,
// config or factory): NULL, the reason in lrvx_last_error_message. A file
// too short for one fold is a handle with zero folds and no error. folds()
// follows lrvx_backtest_result_open_positions: NULL out returns the count,
// otherwise up to max_folds are written and the number written returned.
// Each leg's unrealized_pnl is the open position at the end of that window
// marked at its last price; final_equity = initial_capital + net_pnl +
// unrealized_pnl of that leg.
//
// Grid search -- on the existing LrvxGridSearchHandle. The factory fills the
// LrvxBacktestStats as before and may record the valuation of the
// combination it was handed:
//
//   void   lrvx_grid_search_record_valuation(LrvxGridSearchHandle gs, uint64_t index,
//                                            double unrealized_pnl, double final_equity);
//   double lrvx_grid_search_result_unrealized_pnl(LrvxGridSearchHandle gs, uint64_t index);
//   double lrvx_grid_search_result_final_equity(LrvxGridSearchHandle gs, uint64_t index);
//
// Each lrvx_grid_search_run that runs combinations starts afresh: a
// combination whose factory recorded nothing reads unrealized_pnl 0 and
// final_equity = initialCapital + netPnl of the stats its factory wrote.
//
// Both families follow their neighbours: a NULL handle returns 0 and records
// code 1 naming the function; an index past what the last run produced
// returns 0 and records no error.

#include "lrvx/capi/lrvx_capi.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace
{

constexpr double kEps = 1e-9;
constexpr int64_t kSec = 1'000'000'000;
constexpr int64_t kQtyScale = 100'000'000;
constexpr double kCapital = 10'000.0;

// ── A strategy per window: buys 1 on its first trade; with closeAt > 0 it
// sells it back on that trade and ends flat. ─────────────────────────────

struct WindowStrategy
{
  LrvxStrategyHandle handle{nullptr};
  uint32_t symbol{0};
  int trades{0};
  int closeAt{0};
};

void onTrade(void* ud, const LrvxSymbolContext*, const LrvxTradeData*)
{
  auto* st = static_cast<WindowStrategy*>(ud);
  if (st->trades == 0)
  {
    lrvx_emit_market_buy(st->handle, st->symbol, kQtyScale);
  }
  else if (st->closeAt > 0 && st->trades == st->closeAt)
  {
    lrvx_emit_market_sell(st->handle, st->symbol, kQtyScale);
  }
  ++st->trades;
}

struct Factory
{
  Factory(LrvxRegistryHandle r, uint32_t s, int c = 0) : registry(r), symbol(s), closeAt(c) {}
  Factory(const Factory&) = delete;
  Factory& operator=(const Factory&) = delete;

  LrvxRegistryHandle registry{nullptr};
  uint32_t symbol{0};
  int closeAt{0};
  uint32_t nextId{1};
  std::vector<std::unique_ptr<WindowStrategy>> made;

  ~Factory()
  {
    for (auto& s : made)
    {
      lrvx_strategy_destroy(s->handle);
    }
  }
};

LrvxStrategyHandle makeWindowStrategy(void* ud, uint64_t)
{
  auto* f = static_cast<Factory*>(ud);
  auto st = std::make_unique<WindowStrategy>();
  st->symbol = f->symbol;
  st->closeAt = f->closeAt;
  LrvxStrategyCallbacks cb{};
  cb.on_trade = onTrade;
  cb.user_data = st.get();
  const uint32_t syms[] = {f->symbol};
  st->handle = lrvx_strategy_create(f->nextId++, syms, 1, f->registry, cb);
  LrvxStrategyHandle h = st->handle;
  f->made.push_back(std::move(st));
  return h;
}

class CapiFoldValuation : public ::testing::Test
{
 protected:
  void SetUp() override
  {
    _root = std::filesystem::temp_directory_path() /
            (std::string("lrvx_capi_fold_valuation_") +
             ::testing::UnitTest::GetInstance()->current_test_info()->name() + "_" +
             std::to_string(std::random_device{}()));
    std::filesystem::create_directories(_root);
    _registry = lrvx_registry_create();
    _symbol = lrvx_registry_add_symbol(_registry, "test", "BTCUSDT", 0.01);
    lrvx_clear_last_error();
  }

  void TearDown() override
  {
    lrvx_registry_destroy(_registry);
    std::filesystem::remove_all(_root);
    lrvx_clear_last_error();
  }

  std::string writeCsv(const std::string& name, const std::vector<double>& closes) const
  {
    const auto path = _root / name;
    std::ofstream out(path);
    out << "timestamp,open,high,low,close,volume\n";
    const int64_t t0 = 1'767'225'600 * kSec;
    for (size_t i = 0; i < closes.size(); ++i)
    {
      out << (t0 + static_cast<int64_t>(i) * 60 * kSec) << "," << closes[i] << ","
          << closes[i] << "," << closes[i] << "," << closes[i] << ",1\n";
    }
    return path.string();
  }

  // Three windows of four bars, each flat for three bars and then up: a
  // position bought on a window's first trade is up 10, 30 and 50 at the
  // window's last close.
  std::string rampCsv() const
  {
    return writeCsv("ramp.csv", {100, 100, 100, 110, 200, 200, 200, 230, 300, 300, 300, 350});
  }

  static LrvxWalkForwardConfig twoFolds()
  {
    LrvxWalkForwardConfig cfg{};
    cfg.mode = 1;  // sliding
    cfg.train_size = 4;
    cfg.test_size = 4;
    cfg.step = 4;
    return cfg;
  }

  static std::vector<LrvxWalkForwardFold> folds(LrvxWalkForwardResultHandle r)
  {
    const uint32_t n = lrvx_walk_forward_result_folds(r, nullptr, 0);
    std::vector<LrvxWalkForwardFold> out(n);
    if (n > 0)
    {
      EXPECT_EQ(lrvx_walk_forward_result_folds(r, out.data(), n), n);
    }
    return out;
  }

  std::filesystem::path _root;
  LrvxRegistryHandle _registry{nullptr};
  uint32_t _symbol{0};
};

}  // namespace

// ── The boundary keeps its shape ──────────────────────────────────────────

TEST(CapiFoldValuationAbi, StructsAndAbiVersionAreUnchanged)
{
  EXPECT_EQ(LRVX_CAPI_ABI_VERSION, 6);
  EXPECT_EQ(lrvx_capi_abi_version(), 6u);
  EXPECT_EQ(sizeof(LrvxBacktestStats), 248u)
      << "LrvxBacktestStats changed shape; the valuation belongs in the accessors";
  EXPECT_EQ(sizeof(LrvxWalkForwardFold), 72u + 2 * sizeof(LrvxBacktestStats))
      << "LrvxWalkForwardFold changed shape; the valuation belongs in the accessors";
}

// ── Walk-forward ──────────────────────────────────────────────────────────

TEST_F(CapiFoldValuation, EachFoldLegIsMarkedAtItsWindowsLastClose)
{
  const std::string csv = rampCsv();
  const LrvxWalkForwardConfig cfg = twoFolds();
  Factory f{_registry, _symbol};
  auto* r = lrvx_walk_forward_run_csv_result(_registry, csv.c_str(), "BTCUSDT", 0.0, kCapital,
                                             &cfg, makeWindowStrategy, &f);
  ASSERT_NE(r, nullptr) << lrvx_last_error_message();
  EXPECT_EQ(lrvx_last_error_code(), 0);
  EXPECT_EQ(f.made.size(), 4u) << "one strategy per window: two folds, train and test";

  const auto fs = folds(r);
  ASSERT_EQ(fs.size(), 2u);
  EXPECT_EQ(fs[0].fold_index, 0u);
  EXPECT_EQ(fs[1].fold_index, 1u);
  EXPECT_EQ(fs[0].train_start_bar, 0u);
  EXPECT_EQ(fs[0].test_start_bar, 4u);
  EXPECT_EQ(fs[1].train_start_bar, 4u);
  EXPECT_EQ(fs[1].test_start_bar, 8u);

  // Bought at 100 / 200 / 200 / 300, marked at 110 / 230 / 230 / 350.
  const double trainU[2] = {10.0, 30.0};
  const double testU[2] = {30.0, 50.0};
  for (uint32_t i = 0; i < 2; ++i)
  {
    SCOPED_TRACE("fold " + std::to_string(i));
    EXPECT_EQ(fs[i].train_stats.totalTrades, 0u) << "nothing closed in the window";
    EXPECT_EQ(fs[i].test_stats.totalTrades, 0u);
    EXPECT_NEAR(lrvx_walk_forward_result_train_unrealized_pnl(r, i), trainU[i], kEps);
    EXPECT_NEAR(lrvx_walk_forward_result_test_unrealized_pnl(r, i), testU[i], kEps);
    EXPECT_NEAR(lrvx_walk_forward_result_train_final_equity(r, i),
                fs[i].train_stats.initialCapital + fs[i].train_stats.netPnl + trainU[i], kEps);
    EXPECT_NEAR(lrvx_walk_forward_result_test_final_equity(r, i),
                fs[i].test_stats.initialCapital + fs[i].test_stats.netPnl + testU[i], kEps);
  }
  EXPECT_NEAR(lrvx_walk_forward_result_train_final_equity(r, 0), kCapital + 10.0, kEps);
  EXPECT_NEAR(lrvx_walk_forward_result_test_final_equity(r, 1), kCapital + 50.0, kEps);
  lrvx_walk_forward_result_destroy(r);
}

// The folds the result hands back are the folds lrvx_walk_forward_run_csv
// writes for the same run.
TEST_F(CapiFoldValuation, ResultFoldsMatchRunCsv)
{
  const std::string csv = rampCsv();
  const LrvxWalkForwardConfig cfg = twoFolds();
  Factory fa{_registry, _symbol};
  std::vector<LrvxWalkForwardFold> plain(2);
  ASSERT_EQ(lrvx_walk_forward_run_csv(_registry, csv.c_str(), "BTCUSDT", 0.0004, kCapital, &cfg,
                                      makeWindowStrategy, &fa, plain.data(), 2),
            2u);
  Factory fb{_registry, _symbol};
  auto* r = lrvx_walk_forward_run_csv_result(_registry, csv.c_str(), "BTCUSDT", 0.0004, kCapital,
                                             &cfg, makeWindowStrategy, &fb);
  ASSERT_NE(r, nullptr) << lrvx_last_error_message();
  const auto fs = folds(r);
  ASSERT_EQ(fs.size(), 2u);
  for (size_t i = 0; i < 2; ++i)
  {
    SCOPED_TRACE("fold " + std::to_string(i));
    EXPECT_EQ(fs[i].train_end_bar, plain[i].train_end_bar);
    EXPECT_EQ(fs[i].test_end_bar, plain[i].test_end_bar);
    EXPECT_EQ(fs[i].train_start_ns, plain[i].train_start_ns);
    EXPECT_EQ(fs[i].test_end_ns, plain[i].test_end_ns);
    EXPECT_NEAR(fs[i].train_stats.netPnl, plain[i].train_stats.netPnl, kEps);
    EXPECT_NEAR(fs[i].test_stats.totalFees, plain[i].test_stats.totalFees, kEps);
    EXPECT_NEAR(lrvx_walk_forward_result_test_final_equity(r, static_cast<uint32_t>(i)),
                fs[i].test_stats.initialCapital + fs[i].test_stats.netPnl +
                    lrvx_walk_forward_result_test_unrealized_pnl(r, static_cast<uint32_t>(i)),
                kEps);
  }

  // A short buffer gets the first folds only.
  LrvxWalkForwardFold one{};
  EXPECT_EQ(lrvx_walk_forward_result_folds(r, &one, 1), 1u);
  EXPECT_EQ(one.fold_index, 0u);
  lrvx_walk_forward_result_destroy(r);
}

TEST_F(CapiFoldValuation, FlatWindowsHaveNothingToValue)
{
  const std::string csv = rampCsv();
  const LrvxWalkForwardConfig cfg = twoFolds();
  Factory f{_registry, _symbol, /*closeAt=*/2};
  auto* r = lrvx_walk_forward_run_csv_result(_registry, csv.c_str(), "BTCUSDT", 0.0, kCapital,
                                             &cfg, makeWindowStrategy, &f);
  ASSERT_NE(r, nullptr) << lrvx_last_error_message();
  const auto fs = folds(r);
  ASSERT_EQ(fs.size(), 2u);
  for (uint32_t i = 0; i < 2; ++i)
  {
    SCOPED_TRACE("fold " + std::to_string(i));
    EXPECT_EQ(fs[i].train_stats.totalTrades, 1u);
    EXPECT_EQ(lrvx_walk_forward_result_train_unrealized_pnl(r, i), 0.0);
    EXPECT_EQ(lrvx_walk_forward_result_test_unrealized_pnl(r, i), 0.0);
    EXPECT_NEAR(lrvx_walk_forward_result_train_final_equity(r, i),
                fs[i].train_stats.finalCapital, kEps);
    EXPECT_NEAR(lrvx_walk_forward_result_test_final_equity(r, i),
                fs[i].test_stats.finalCapital, kEps);
  }
  lrvx_walk_forward_result_destroy(r);
}

TEST_F(CapiFoldValuation, FoldIndexPastTheEndReadsZeroWithoutAnError)
{
  const std::string csv = rampCsv();
  const LrvxWalkForwardConfig cfg = twoFolds();
  Factory f{_registry, _symbol};
  auto* r = lrvx_walk_forward_run_csv_result(_registry, csv.c_str(), "BTCUSDT", 0.0, kCapital,
                                             &cfg, makeWindowStrategy, &f);
  ASSERT_NE(r, nullptr) << lrvx_last_error_message();
  for (const uint32_t i : {2u, 3u, 0xFFFFFFFFu})
  {
    SCOPED_TRACE("index " + std::to_string(i));
    lrvx_clear_last_error();
    EXPECT_EQ(lrvx_walk_forward_result_train_unrealized_pnl(r, i), 0.0);
    EXPECT_EQ(lrvx_walk_forward_result_train_final_equity(r, i), 0.0);
    EXPECT_EQ(lrvx_walk_forward_result_test_unrealized_pnl(r, i), 0.0);
    EXPECT_EQ(lrvx_walk_forward_result_test_final_equity(r, i), 0.0);
    EXPECT_EQ(lrvx_last_error_code(), 0);
  }
  lrvx_walk_forward_result_destroy(r);
}

TEST_F(CapiFoldValuation, TooFewBarsIsAnEmptyResultNotAnError)
{
  const std::string csv = writeCsv("short.csv", {100, 101, 102});
  const LrvxWalkForwardConfig cfg = twoFolds();
  Factory f{_registry, _symbol};
  auto* r = lrvx_walk_forward_run_csv_result(_registry, csv.c_str(), "BTCUSDT", 0.0, kCapital,
                                             &cfg, makeWindowStrategy, &f);
  ASSERT_NE(r, nullptr) << lrvx_last_error_message();
  EXPECT_EQ(lrvx_last_error_code(), 0);
  EXPECT_EQ(lrvx_walk_forward_result_folds(r, nullptr, 0), 0u);
  EXPECT_EQ(lrvx_walk_forward_result_test_unrealized_pnl(r, 0), 0.0);
  EXPECT_EQ(lrvx_last_error_code(), 0);
  EXPECT_TRUE(f.made.empty());
  lrvx_walk_forward_result_destroy(r);
}

TEST_F(CapiFoldValuation, RunFailuresReturnNullWithTheReason)
{
  const LrvxWalkForwardConfig cfg = twoFolds();
  Factory f{_registry, _symbol};
  const std::string missing = (_root / "no_such_file.csv").string();
  EXPECT_EQ(lrvx_walk_forward_run_csv_result(_registry, missing.c_str(), "BTCUSDT", 0.0, kCapital,
                                             &cfg, makeWindowStrategy, &f),
            nullptr);
  EXPECT_NE(lrvx_last_error_code(), 0);
  EXPECT_NE(std::string(lrvx_last_error_message()).find(missing), std::string::npos)
      << lrvx_last_error_message();

  lrvx_clear_last_error();
  const std::string csv = rampCsv();
  EXPECT_EQ(lrvx_walk_forward_run_csv_result(_registry, csv.c_str(), "NOPE", 0.0, kCapital, &cfg,
                                             makeWindowStrategy, &f),
            nullptr);
  EXPECT_NE(lrvx_last_error_code(), 0);
  EXPECT_NE(std::string(lrvx_last_error_message()).find("NOPE"), std::string::npos)
      << lrvx_last_error_message();

  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_walk_forward_run_csv_result(nullptr, csv.c_str(), "BTCUSDT", 0.0, kCapital, &cfg,
                                             makeWindowStrategy, &f),
            nullptr);
  EXPECT_EQ(lrvx_last_error_code(), 1);

  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_walk_forward_run_csv_result(_registry, csv.c_str(), "BTCUSDT", 0.0, kCapital,
                                             &cfg, nullptr, &f),
            nullptr);
  EXPECT_NE(lrvx_last_error_code(), 0);
  EXPECT_TRUE(f.made.empty()) << "a failed run builds no strategy";
}

// ── Grid search ───────────────────────────────────────────────────────────

namespace
{

// Writes stats whose net_pnl is 10 * the first parameter and, unless told
// not to, records an unrealized PnL of the second parameter.
struct GridFactory
{
  LrvxGridSearchHandle gs{nullptr};
  bool record{true};
  uint64_t calls{0};
};

int gridFactory(void* ud, uint64_t index, const double* params, uint32_t n,
                LrvxBacktestStats* out)
{
  auto* g = static_cast<GridFactory*>(ud);
  ++g->calls;
  out->initialCapital = kCapital;
  out->netPnl = 10.0 * params[0];
  out->totalPnl = out->netPnl;
  out->finalCapital = kCapital + out->netPnl;
  if (g->record && n > 1)
  {
    lrvx_grid_search_record_valuation(g->gs, index, params[1],
                                      kCapital + out->netPnl + params[1]);
  }
  return 1;
}

LrvxGridSearchHandle twoByTwo()
{
  auto* gs = lrvx_grid_search_create();
  const double a[] = {1.0, 2.0};
  const double b[] = {-3.0, 7.0};
  lrvx_grid_search_add_axis(gs, a, 2);
  lrvx_grid_search_add_axis(gs, b, 2);
  return gs;
}

}  // namespace

TEST(CapiGridValuation, ARecordedValuationIsReadBackPerCombination)
{
  auto* gs = twoByTwo();
  GridFactory g;
  g.gs = gs;
  std::vector<LrvxBacktestStats> stats(4);
  lrvx_clear_last_error();
  ASSERT_EQ(lrvx_grid_search_run(gs, gridFactory, &g, stats.data(), 4), 4u);
  // Last axis fastest: (1,-3), (1,7), (2,-3), (2,7).
  const double unrealized[4] = {-3.0, 7.0, -3.0, 7.0};
  const double net[4] = {10.0, 10.0, 20.0, 20.0};
  for (uint64_t i = 0; i < 4; ++i)
  {
    SCOPED_TRACE("combination " + std::to_string(i));
    EXPECT_NEAR(stats[i].netPnl, net[i], kEps);
    EXPECT_NEAR(lrvx_grid_search_result_unrealized_pnl(gs, i), unrealized[i], kEps);
    EXPECT_NEAR(lrvx_grid_search_result_final_equity(gs, i), kCapital + net[i] + unrealized[i],
                kEps);
  }
  EXPECT_EQ(lrvx_last_error_code(), 0);
  lrvx_grid_search_destroy(gs);
}

TEST(CapiGridValuation, ACombinationThatRecordedNothingHasNoOpenPosition)
{
  auto* gs = twoByTwo();
  GridFactory g;
  g.gs = gs;
  g.record = false;
  std::vector<LrvxBacktestStats> stats(4);
  ASSERT_EQ(lrvx_grid_search_run(gs, gridFactory, &g, stats.data(), 4), 4u);
  for (uint64_t i = 0; i < 4; ++i)
  {
    SCOPED_TRACE("combination " + std::to_string(i));
    EXPECT_EQ(lrvx_grid_search_result_unrealized_pnl(gs, i), 0.0);
    EXPECT_NEAR(lrvx_grid_search_result_final_equity(gs, i),
                stats[i].initialCapital + stats[i].netPnl, kEps)
        << "final_equity is initial_capital + net_pnl when nothing is open";
  }
  lrvx_grid_search_destroy(gs);
}

TEST(CapiGridValuation, EachRunStartsAfresh)
{
  auto* gs = twoByTwo();
  GridFactory g;
  g.gs = gs;
  std::vector<LrvxBacktestStats> stats(4);
  ASSERT_EQ(lrvx_grid_search_run(gs, gridFactory, &g, stats.data(), 4), 4u);
  ASSERT_NEAR(lrvx_grid_search_result_unrealized_pnl(gs, 1), 7.0, kEps);

  g.record = false;
  ASSERT_EQ(lrvx_grid_search_run(gs, gridFactory, &g, stats.data(), 4), 4u);
  EXPECT_EQ(lrvx_grid_search_result_unrealized_pnl(gs, 1), 0.0)
      << "the first run's record does not carry into the second";
  EXPECT_NEAR(lrvx_grid_search_result_final_equity(gs, 1), kCapital + 10.0, kEps);

  // A run cut short by max_results produces fewer results; the rest are past
  // the end.
  g.record = true;
  ASSERT_EQ(lrvx_grid_search_run(gs, gridFactory, &g, stats.data(), 2), 2u);
  EXPECT_NEAR(lrvx_grid_search_result_unrealized_pnl(gs, 1), 7.0, kEps);
  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_grid_search_result_unrealized_pnl(gs, 2), 0.0);
  EXPECT_EQ(lrvx_grid_search_result_final_equity(gs, 2), 0.0);
  EXPECT_EQ(lrvx_last_error_code(), 0);
  lrvx_grid_search_destroy(gs);
}

TEST(CapiGridValuation, IndexPastTheEndReadsZeroWithoutAnError)
{
  auto* gs = twoByTwo();
  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_grid_search_result_unrealized_pnl(gs, 0), 0.0) << "nothing has run yet";
  EXPECT_EQ(lrvx_grid_search_result_final_equity(gs, 0), 0.0);
  EXPECT_EQ(lrvx_last_error_code(), 0);

  GridFactory g;
  g.gs = gs;
  std::vector<LrvxBacktestStats> stats(4);
  ASSERT_EQ(lrvx_grid_search_run(gs, gridFactory, &g, stats.data(), 4), 4u);
  for (const uint64_t i : {uint64_t{4}, uint64_t{1} << 40, ~uint64_t{0}})
  {
    lrvx_clear_last_error();
    EXPECT_EQ(lrvx_grid_search_result_unrealized_pnl(gs, i), 0.0);
    EXPECT_EQ(lrvx_grid_search_result_final_equity(gs, i), 0.0);
    // A record outside the run's combinations is dropped, not an error.
    lrvx_grid_search_record_valuation(gs, i, 1.0, 2.0);
    EXPECT_EQ(lrvx_last_error_code(), 0);
  }
  EXPECT_NEAR(lrvx_grid_search_result_unrealized_pnl(gs, 3), 7.0, kEps)
      << "the out-of-range records touched no combination";
  lrvx_grid_search_destroy(gs);
}

// ── NULL handles ──────────────────────────────────────────────────────────

// Each new function records the documented code for a NULL handle (1) and
// names itself in the message, as the neighbouring accessors do.
TEST(CapiFoldValuationNull, EachNullHandleRecordsCodeOneNamingTheFunction)
{
  struct Case
  {
    const char* fn;
    void (*call)();
  };
  const Case cases[] = {
      {"lrvx_walk_forward_result_folds",
       []
       {
         LrvxWalkForwardFold out[1]{};
         EXPECT_EQ(lrvx_walk_forward_result_folds(nullptr, out, 1), 0u);
       }},
      {"lrvx_walk_forward_result_train_unrealized_pnl",
       []
       { EXPECT_EQ(lrvx_walk_forward_result_train_unrealized_pnl(nullptr, 0), 0.0); }},
      {"lrvx_walk_forward_result_train_final_equity",
       []
       { EXPECT_EQ(lrvx_walk_forward_result_train_final_equity(nullptr, 0), 0.0); }},
      {"lrvx_walk_forward_result_test_unrealized_pnl",
       []
       { EXPECT_EQ(lrvx_walk_forward_result_test_unrealized_pnl(nullptr, 0), 0.0); }},
      {"lrvx_walk_forward_result_test_final_equity",
       []
       { EXPECT_EQ(lrvx_walk_forward_result_test_final_equity(nullptr, 0), 0.0); }},
      {"lrvx_grid_search_record_valuation",
       []
       { lrvx_grid_search_record_valuation(nullptr, 0, 1.0, 2.0); }},
      {"lrvx_grid_search_result_unrealized_pnl",
       []
       { EXPECT_EQ(lrvx_grid_search_result_unrealized_pnl(nullptr, 0), 0.0); }},
      {"lrvx_grid_search_result_final_equity",
       []
       { EXPECT_EQ(lrvx_grid_search_result_final_equity(nullptr, 0), 0.0); }},
  };
  for (const auto& c : cases)
  {
    lrvx_clear_last_error();
    ASSERT_EQ(lrvx_last_error_code(), 0);
    c.call();
    EXPECT_EQ(lrvx_last_error_code(), 1) << c.fn;
    const char* msg = lrvx_last_error_message();
    ASSERT_NE(msg, nullptr) << c.fn;
    EXPECT_NE(std::string(msg).find(c.fn), std::string::npos) << c.fn << ": " << msg;
  }

  // Destroying NULL is not an error, as for every _destroy.
  lrvx_clear_last_error();
  lrvx_walk_forward_result_destroy(nullptr);
  EXPECT_EQ(lrvx_last_error_code(), 0);
}
