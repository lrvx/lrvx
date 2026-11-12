/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Per-trade fee attribution and the drawdown the summary reports.
//
// Both used to disagree with the totals sitting next to them in the same
// object. The fee split overflowed and could come out negative, so equity
// after a flat round trip landed above the starting capital while
// `totalFees` stayed right. The drawdown percentage divided an absolute
// maximum captured mid-run by the peak equity as it stood at the end, so the
// summary card and the equity-curve timeline on the same report page printed
// different numbers.

#include "lrvx/backtest/backtest_result.h"

#include <gtest/gtest.h>

#include <algorithm>

using namespace lrvx;

namespace
{

Fill makeFill(uint64_t id, Side side, double price, double qty, int64_t ts)
{
  Fill f;
  f.orderId = id;
  f.symbol = 1;
  f.side = side;
  f.price = Price::fromDouble(price);
  f.quantity = Quantity::fromDouble(qty);
  f.timestampNs = UnixNanos::fromRaw(ts);
  return f;
}

}  // namespace

// 5 BTC at 100,000 with 5 basis points a side: 500 of fee on the way in and
// 500 on the way out. The per-side fee raw times the quantity raw is
// 5e10 * 5e8 = 2.5e19, past the int64 ceiling, and the wrapped result was
// -237.87 -- a fee the backtest paid the strategy.
TEST(BacktestFeeAttribution, PerTradeFeeSurvivesLargeNotional)
{
  BacktestConfig config;
  config.feeRate = 0.0005;
  BacktestResult result(config);

  result.recordFill(makeFill(1, Side::BUY, 100000.0, 5.0, 1000));
  result.recordFill(makeFill(2, Side::SELL, 100000.0, 5.0, 2000));

  ASSERT_EQ(result.trades().size(), 1u);
  const double tradeFee = result.trades()[0].fee.toDouble();

  EXPECT_GT(tradeFee, 0.0);
  EXPECT_NEAR(tradeFee, 500.0, 0.01);

  const auto stats = result.computeStats();
  EXPECT_NEAR(stats.totalFees, 500.0, 0.01);
}

// The threshold the audit measured: per-side fee in quote currency times
// quantity above about 922. 6.7 BTC at 4 basis points sits under it and
// always worked; 6.8 BTC sits over it and came back as -270.55.
TEST(BacktestFeeAttribution, FeeStaysPositiveEitherSideOfTheOldThreshold)
{
  for (double qty : {6.7, 6.8, 30.0, 100.0})
  {
    BacktestConfig config;
    config.feeRate = 0.0004;
    BacktestResult result(config);

    result.recordFill(makeFill(1, Side::BUY, 50000.0, qty, 1000));
    result.recordFill(makeFill(2, Side::SELL, 50000.0, qty, 2000));

    ASSERT_EQ(result.trades().size(), 1u) << "qty=" << qty;
    const double expected = 2.0 * 50000.0 * qty * 0.0004;
    EXPECT_NEAR(result.trades()[0].fee.toDouble(), expected, 0.02) << "qty=" << qty;
  }
}

// The fee charged on the trades has to match the fee charged on the equity
// curve, or every metric built on the curve -- Sharpe, Sortino, drawdown,
// Calmar, the HTML report -- runs on numbers the summary never shows.
TEST(BacktestFeeAttribution, EquityCurveAgreesWithFinalCapital)
{
  BacktestConfig config;
  config.initialCapital = 10'000'000.0;
  config.feeRate = 0.0004;
  BacktestResult result(config);

  result.recordFill(makeFill(1, Side::BUY, 50000.0, 10.0, 1000));
  result.recordFill(makeFill(2, Side::SELL, 50000.0, 10.0, 2000));

  const auto stats = result.computeStats();
  ASSERT_FALSE(result.equityCurve().empty());
  const double internalEquity = result.equityCurve().back().equity;

  EXPECT_NEAR(stats.finalCapital, internalEquity, 0.01);
  EXPECT_LT(stats.finalCapital, config.initialCapital);
}

// A million units of a one-dollar asset: the product is 5e8 times past the
// ceiling, and the old form reported a fee of -0.0017 on a true 1000.
TEST(BacktestFeeAttribution, SmallPriceLargeQuantity)
{
  BacktestConfig config;
  config.feeRate = 0.0005;
  BacktestResult result(config);

  result.recordFill(makeFill(1, Side::BUY, 1.0, 1'000'000.0, 1000));
  result.recordFill(makeFill(2, Side::SELL, 1.0, 1'000'000.0, 2000));

  ASSERT_EQ(result.trades().size(), 1u);
  EXPECT_NEAR(result.trades()[0].fee.toDouble(), 1000.0, 0.02);
}

// 1,000,000 down to 700,000 and up to 3,000,000. The drawdown was 30% when it
// happened. Measuring it against the 3,000,000 peak the run ended on reported
// 10%, and the Calmar ratio was inflated by exactly the same factor.
TEST(BacktestDrawdown, MaxDrawdownPctUsesThePeakThatStoodAtTheTime)
{
  BacktestConfig config;
  config.initialCapital = 1'000'000.0;
  config.feeRate = 0.0;
  BacktestResult result(config);

  result.recordFill(makeFill(1, Side::BUY, 200.0, 3000.0, 1000));
  result.recordFill(makeFill(2, Side::SELL, 100.0, 3000.0, 2000));  // -300,000
  result.recordFill(makeFill(3, Side::BUY, 100.0, 23000.0, 3000));
  result.recordFill(makeFill(4, Side::SELL, 200.0, 23000.0, 4000));  // +2,300,000

  const auto stats = result.computeStats();
  EXPECT_NEAR(stats.maxDrawdown, 300000.0, 1.0);
  EXPECT_NEAR(stats.maxDrawdownPct, 30.0, 0.01);

  double curveMax = 0.0;
  for (const auto& pt : result.equityCurve())
  {
    curveMax = std::max(curveMax, pt.drawdownPct);
  }
  EXPECT_NEAR(stats.maxDrawdownPct, curveMax, 1e-9);
}

// A run that ends with a position open. totalFees counts every fill, so it
// holds the open entry's fee; trades() and the equity curve hold closed trades
// only. netPnl is therefore the closed trades' net minus that fee, and the
// open position's unrealized PnL is in none of them. fills() is where the
// open position shows. This pins the rule docs/reference/api/backtest/
// backtest_result.md states.
TEST(BacktestFeeAttribution, OpenPositionAtTheEndChargesItsEntryFeeToNetPnlOnly)
{
  BacktestConfig config;
  config.initialCapital = 10'000.0;
  config.feeRate = 0.0004;
  BacktestResult result(config);

  result.recordFill(makeFill(1, Side::BUY, 78000.0, 0.01, 1000));
  result.recordFill(makeFill(2, Side::SELL, 79000.0, 0.01, 2000));
  result.recordFill(makeFill(3, Side::BUY, 78000.0, 0.01, 3000));

  ASSERT_EQ(result.trades().size(), 1u);
  ASSERT_EQ(result.equityCurve().size(), 1u);
  const auto& trade = result.trades()[0];
  const double closedNet = trade.pnl.toDouble() - trade.fee.toDouble();
  const double openEntryFee = 0.01 * 78000.0 * 0.0004;  // 0.312

  const auto stats = result.computeStats();
  EXPECT_NEAR(stats.totalFees, trade.fee.toDouble() + openEntryFee, 1e-9);
  EXPECT_NEAR(stats.netPnl, closedNet - openEntryFee, 1e-9);
  EXPECT_NEAR(stats.finalCapital, 10'000.0 + closedNet - openEntryFee, 1e-9);
  EXPECT_NEAR(result.equityCurve().back().equity, 10'000.0 + closedNet, 1e-9);

  double net = 0.0;
  for (const auto& f : result.fills())
  {
    net += (f.side == Side::BUY ? 1.0 : -1.0) * f.quantity.toDouble();
  }
  EXPECT_NEAR(net, 0.01, 1e-12);
}

// With no closed trade the statistics are all zero, the open entry's fee
// included.
TEST(BacktestFeeAttribution, NoClosedTradeReportsNoFees)
{
  BacktestConfig config;
  config.initialCapital = 10'000.0;
  config.feeRate = 0.0004;
  BacktestResult result(config);

  result.recordFill(makeFill(1, Side::BUY, 78000.0, 0.01, 1000));

  const auto stats = result.computeStats();
  EXPECT_EQ(stats.totalTrades, 0u);
  EXPECT_EQ(stats.totalFees, 0.0);
  EXPECT_EQ(stats.netPnl, 0.0);
  EXPECT_EQ(stats.finalCapital, 10'000.0);
  EXPECT_TRUE(result.trades().empty());
  EXPECT_TRUE(result.equityCurve().empty());
}
