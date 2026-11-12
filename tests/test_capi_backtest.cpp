/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/capi/lrvx_capi.h"

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <string>

TEST(CapiBacktest, SlippageFixedBps)
{
  auto exec = lrvx_simulated_executor_create();
  lrvx_simulated_executor_set_default_slippage(exec, LRVX_SLIPPAGE_FIXED_BPS, 0, 0.0, 100.0, 0.0);
  lrvx_simulated_executor_on_best_levels(exec, 1, 100.0, 10.0, 100.0, 10.0);
  lrvx_simulated_executor_submit_order(exec, /*id=*/1, /*side=buy=*/0, 0.0, 1.0, /*type=market=*/1, 1);
  ASSERT_EQ(lrvx_simulated_executor_fill_count(exec), 1u);
  lrvx_simulated_executor_destroy(exec);
}

// An out-of-range model code is refused, not read as some other model: the
// error names the valid codes and the profile installed before stays.
TEST(CapiBacktest, SlippageRejectsUnknownModelCode)
{
  auto exec = lrvx_simulated_executor_create();
  lrvx_simulated_executor_set_default_slippage(exec, LRVX_SLIPPAGE_FIXED_BPS, 0, 0.0, 100.0, 0.0);
  ASSERT_EQ(lrvx_last_error_code(), 0);

  for (int32_t bad : {4, -1, 255})
  {
    lrvx_clear_last_error();
    lrvx_simulated_executor_set_default_slippage(exec, bad, 0, 0.0, 0.0, 0.0);
    EXPECT_NE(lrvx_last_error_code(), 0) << "model " << bad;
    const std::string msg = lrvx_last_error_message();
    EXPECT_NE(msg.find("1 (fixed_ticks)"), std::string::npos) << msg;
    EXPECT_NE(msg.find("3 (volume_impact)"), std::string::npos) << msg;
  }
  lrvx_clear_last_error();
  lrvx_simulated_executor_set_symbol_slippage(exec, 1, 7, 0, 0.0, 0.0, 0.0);
  EXPECT_NE(lrvx_last_error_code(), 0);
  lrvx_clear_last_error();

  lrvx_simulated_executor_on_best_levels(exec, 1, 100.0, 10.0, 100.0, 10.0);
  lrvx_simulated_executor_submit_order(exec, /*id=*/1, /*side=buy=*/0, 0.0, 1.0, /*type=market=*/1, 1);
  LrvxFill fill{};
  ASSERT_EQ(lrvx_simulated_executor_get_fills(exec, &fill, 1), 1u);
  EXPECT_DOUBLE_EQ(static_cast<double>(fill.price_raw) / 1e8, 101.0)
      << "the fixed_bps profile installed before the refused calls still applies";
  lrvx_simulated_executor_destroy(exec);
}

// Same for the queue model: an out-of-range code is refused with the valid
// codes and names, and the model installed before stays.
TEST(CapiBacktest, QueueModelRejectsUnknownModelCode)
{
  auto exec = lrvx_simulated_executor_create();
  for (int32_t bad : {7, -1, 255})
  {
    lrvx_clear_last_error();
    lrvx_simulated_executor_set_queue_model(exec, bad, 1);
    EXPECT_NE(lrvx_last_error_code(), 0) << "model " << bad;
    const std::string msg = lrvx_last_error_message();
    EXPECT_NE(msg.find("1 (tob)"), std::string::npos) << msg;
    EXPECT_NE(msg.find("6 (pro_rata_with_priority)"), std::string::npos) << msg;
  }
  for (int32_t good = 0; good <= 6; ++good)
  {
    lrvx_clear_last_error();
    lrvx_simulated_executor_set_queue_model(exec, good, 1);
    EXPECT_EQ(lrvx_last_error_code(), 0) << "model " << good;
  }
  lrvx_simulated_executor_destroy(exec);
}

TEST(CapiBacktest, TopOfBookMovesTheTouchAndKeepsDepth)
{
  auto exec = lrvx_simulated_executor_create();
  double bp[] = {99.0};
  double bq[] = {5.0};
  double ap[] = {101.0, 102.0};
  double aq[] = {1.0, 5.0};
  lrvx_simulated_executor_on_book_snapshot(exec, 1, bp, bq, 1, ap, aq, 2);
  lrvx_simulated_executor_on_top_of_book(exec, 1, /*has_bid=*/1, 99.5, 5.0,
                                         /*has_ask=*/1, 100.5, 2.0);

  // Bigger than the touch level, so the walk starts at the moved touch and
  // continues into the snapshot's surviving 101.
  lrvx_simulated_executor_submit_order(exec, 1, /*side=buy=*/0, 0.0, 3.0, /*type=market=*/1, 1);
  ASSERT_EQ(lrvx_simulated_executor_fill_count(exec), 1u);
  LrvxFill f{};
  ASSERT_EQ(lrvx_simulated_executor_get_fills(exec, &f, 1), 1u);
  // VWAP of 100.5 x 2 + 101 x 1 — not the stale 101 touch, not a
  // one-level book.
  EXPECT_NEAR(static_cast<double>(f.price_raw) / 1e8, (100.5 * 2 + 101.0) / 3.0, 1e-6);
  lrvx_simulated_executor_destroy(exec);
}

// The flag is the only emptiness signal: has_bid == 0 feeds no bid side,
// so a market sell has nothing to match and no trade to fall back on.
TEST(CapiBacktest, TopOfBookEmptySideMeansEmptySide)
{
  auto exec = lrvx_simulated_executor_create();
  lrvx_simulated_executor_on_top_of_book(exec, 1, /*has_bid=*/0, 0.0, 0.0,
                                         /*has_ask=*/1, 100.5, 2.0);
  lrvx_simulated_executor_submit_order(exec, 1, /*side=sell=*/1, 0.0, 1.0, /*type=market=*/1, 1);
  EXPECT_EQ(lrvx_simulated_executor_fill_count(exec), 0u);
  lrvx_simulated_executor_destroy(exec);
}

// on_best_levels is routed to the same touch path: the depth behind the
// quoted best survives, so a buy bigger than the best level walks into it
// instead of printing everything at a book one level deep.
TEST(CapiBacktest, BestLevelsKeepDeeperDepth)
{
  auto exec = lrvx_simulated_executor_create();
  double bp[] = {99.0};
  double bq[] = {5.0};
  double ap[] = {101.0, 102.0};
  double aq[] = {1.0, 5.0};
  lrvx_simulated_executor_on_book_snapshot(exec, 1, bp, bq, 1, ap, aq, 2);
  lrvx_simulated_executor_on_best_levels(exec, 1, 99.5, 5.0, 100.5, 1.0);

  lrvx_simulated_executor_submit_order(exec, 1, /*side=buy=*/0, 0.0, 3.0, /*type=market=*/1, 1);
  ASSERT_EQ(lrvx_simulated_executor_fill_count(exec), 1u);
  LrvxFill f{};
  ASSERT_EQ(lrvx_simulated_executor_get_fills(exec, &f, 1), 1u);
  // VWAP of 100.5 x 1 + 101 x 1 + 102 x 1, on the fixed-point grid.
  EXPECT_NEAR(static_cast<double>(f.price_raw) / 1e8, (100.5 + 101.0 + 102.0) / 3.0, 1e-6);
  lrvx_simulated_executor_destroy(exec);
}

TEST(CapiBacktest, QueueFillOnTradeAhead)
{
  auto exec = lrvx_simulated_executor_create();
  lrvx_simulated_executor_set_queue_model(exec, LRVX_QUEUE_TOB, 1);

  // Best ask at 101, no bids. Buy limit at 99 sits behind everyone else's
  // bid-side queue of size 0 (fresh level), waiting for a sell print at 99.
  lrvx_simulated_executor_on_best_levels(exec, 1, 99.0, 0.0, 101.0, 5.0);

  lrvx_simulated_executor_submit_order(exec, 42, /*buy=*/0, 99.0, 2.0, /*type=limit=*/0, 1);
  EXPECT_EQ(lrvx_simulated_executor_fill_count(exec), 0u);

  lrvx_simulated_executor_on_trade_qty(exec, 1, 99.0, 2.0, /*is_buy=*/0);
  EXPECT_EQ(lrvx_simulated_executor_fill_count(exec), 1u);
  lrvx_simulated_executor_destroy(exec);
}

TEST(CapiBacktest, ResultStatsAndEquityCurve)
{
  auto result = lrvx_backtest_result_create(10000.0, 0.0, 1, 0.0, 0.0, 252.0);

  lrvx_backtest_result_record_fill(result, 1, 1, /*buy=*/0, 100.0, 1.0, 1000);
  lrvx_backtest_result_record_fill(result, 2, 1, /*sell=*/1, 110.0, 1.0, 2000);
  lrvx_backtest_result_record_fill(result, 3, 1, 0, 100.0, 1.0, 3000);
  lrvx_backtest_result_record_fill(result, 4, 1, 1, 95.0, 1.0, 4000);

  LrvxBacktestStats s{};
  lrvx_backtest_result_stats(result, &s);
  EXPECT_EQ(s.totalTrades, 2u);
  EXPECT_EQ(s.winningTrades, 1u);
  EXPECT_EQ(s.losingTrades, 1u);
  EXPECT_NEAR(s.totalPnl, 5.0, 1e-6);

  const uint32_t n = lrvx_backtest_result_equity_curve(result, nullptr, 0);
  EXPECT_EQ(n, 2u);
  LrvxEquityPoint pts[2];
  EXPECT_EQ(lrvx_backtest_result_equity_curve(result, pts, 2), 2u);
  EXPECT_GT(pts[0].equity, 10000.0);

  auto path = std::filesystem::temp_directory_path() / "lrvx_capi_eq.csv";
  EXPECT_EQ(lrvx_backtest_result_write_equity_curve_csv(result, path.c_str()), 1);
  std::ifstream in(path);
  ASSERT_TRUE(in);
  std::string line;
  std::getline(in, line);
  EXPECT_EQ(line, "timestamp_ns,equity,drawdown_pct");
  std::filesystem::remove(path);

  lrvx_backtest_result_destroy(result);
}
