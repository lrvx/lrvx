/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// The reference backtest: docs/explanation/reference-backtest.md fixes one
// strategy, one data file and one configuration, and lists what the engine
// produces for them. This test is that list in C++; the Python test suite
// pins the same numbers through the binding, so the two paths are held to
// one answer. A change that moves any value here is a change to the
// execution model and updates the page with it.

#include "lrvx/aggregator/events/bar_event.h"
#include "lrvx/backtest/backtest_config.h"
#include "lrvx/backtest/backtest_runner.h"
#include "lrvx/engine/symbol_registry.h"
#include "lrvx/indicator/sma.h"
#include "lrvx/strategy/strategy.h"

#include <gtest/gtest.h>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace lrvx;

namespace
{

constexpr int64_t kMinuteNs = 60'000'000'000LL;

std::vector<BarEvent> loadBars(const std::string& path, SymbolId sym)
{
  std::ifstream in(path);
  EXPECT_TRUE(in.is_open()) << "cannot open " << path;
  std::vector<BarEvent> bars;
  std::string line;
  std::getline(in, line);  // header: timestamp,open,high,low,close,volume
  while (std::getline(in, line))
  {
    long long tsMs = 0;
    double o = 0, h = 0, l = 0, c = 0, v = 0;
    if (std::sscanf(line.c_str(), "%lld,%lf,%lf,%lf,%lf,%lf", &tsMs, &o, &h, &l, &c, &v) != 6)
    {
      continue;
    }
    BarEvent ev{};
    ev.symbol = sym;
    ev.barType = BarType::Time;
    ev.barTypeParam = static_cast<uint64_t>(kMinuteNs);
    ev.bar.open = Price::fromDouble(o);
    ev.bar.high = Price::fromDouble(h);
    ev.bar.low = Price::fromDouble(l);
    ev.bar.close = Price::fromDouble(c);
    ev.bar.volume = Volume::fromDouble(v);
    const int64_t startNs = tsMs * 1'000'000LL;
    ev.bar.startTime = TimePoint{std::chrono::nanoseconds{startNs}};
    ev.bar.endTime = TimePoint{std::chrono::nanoseconds{startNs + kMinuteNs}};
    ev.bar.reason = BarCloseReason::Threshold;
    bars.push_back(ev);
  }
  return bars;
}

// Long 0.01 while SMA(10) of the close is above SMA(40), flat otherwise.
class SmaCross : public Strategy
{
 public:
  using Strategy::Strategy;

 protected:
  void onSymbolBar(SymbolContext& ctx, const BarEvent& ev) override
  {
    const double close = ev.bar.close.toDouble();
    _fast.update(close);
    _slow.update(close);
    if (!_fast.ready() || !_slow.ready())
    {
      return;
    }
    const double f = _fast.value();
    const double s = _slow.value();
    if (f > s && ctx.position.raw() == 0)
    {
      emitMarketBuy(ev.symbol, Quantity::fromDouble(0.01));
    }
    else if (f < s && ctx.position.raw() > 0)
    {
      emitClosePosition(ev.symbol);
    }
  }

 private:
  indicator::SMA _fast{10};
  indicator::SMA _slow{40};
};

struct ReferenceRun
{
  std::vector<BarEvent> bars;
  BacktestResult result;
  BacktestStats stats;
};

ReferenceRun runReference()
{
  SymbolRegistry reg;
  SymbolInfo info;
  info.exchange = "exchange";
  info.symbol = "BTCUSDT";
  info.type = InstrumentType::Spot;
  info.tickSize = Price::fromDouble(0.01);
  const SymbolId sym = reg.registerSymbol(info);

  BacktestConfig config;
  config.feeRate = 0.0004;
  config.initialCapital = 10000.0;

  SmaCross strat(1, std::vector<SymbolId>{sym}, reg);
  BacktestRunner runner(config);
  runner.setStrategy(&strat);

  ReferenceRun run;
  run.bars = loadBars(std::string(LRVX_DOCS_DIR) + "/examples/data/btcusdt_1m.csv", sym);
  EXPECT_EQ(run.bars.size(), 3000u);
  run.result = runner.runBars(run.bars);
  run.stats = run.result.computeStats();
  return run;
}

}  // namespace

TEST(ReferenceBacktest, StatisticsArePinned)
{
  const ReferenceRun run = runReference();
  const BacktestStats& s = run.stats;
  EXPECT_EQ(s.totalTrades, 47u);
  EXPECT_EQ(s.winningTrades, 16u);
  EXPECT_EQ(s.losingTrades, 31u);
  EXPECT_NEAR(s.grossProfit, 28.0608, 1e-9);
  EXPECT_NEAR(s.totalPnl, -14.7146, 1e-9);
  EXPECT_NEAR(s.totalFees, 28.37314883, 1e-9);
  EXPECT_NEAR(s.netPnl, -43.08774883, 1e-9);
  EXPECT_NEAR(s.finalCapital, 9956.91225117, 1e-9);
  EXPECT_NEAR(s.returnPct, -0.4308774883, 1e-9);
}

TEST(ReferenceBacktest, FirstAndLastTradeArePinned)
{
  const ReferenceRun run = runReference();
  const auto& trades = run.result.trades();
  ASSERT_EQ(trades.size(), 47u);

  const TradeRecord& first = trades.front();
  // A trade record carries the side of the fill that closed it: SELL for
  // every round trip of this long-only strategy.
  EXPECT_EQ(first.side, Side::SELL);
  EXPECT_EQ(first.entryTimeNs.raw(), 1776609420000000000LL);
  EXPECT_NEAR(first.entryPrice.toDouble(), 75822.85, 1e-9);
  EXPECT_EQ(first.exitTimeNs.raw(), 1776611280000000000LL);
  EXPECT_NEAR(first.exitPrice.toDouble(), 75909.76, 1e-9);
  EXPECT_NEAR(first.pnl.toDouble(), 0.8691, 1e-9);
  EXPECT_NEAR(first.fee.toDouble(), 0.60693044, 1e-9);

  const TradeRecord& last = trades.back();
  EXPECT_EQ(last.entryTimeNs.raw(), 1776781140000000000LL);
  EXPECT_NEAR(last.entryPrice.toDouble(), 76185.83, 1e-9);
  EXPECT_EQ(last.exitTimeNs.raw(), 1776783420000000000LL);
  EXPECT_NEAR(last.exitPrice.toDouble(), 76005.21, 1e-9);
  EXPECT_NEAR(last.pnl.toDouble(), -1.8062, 1e-9);
  EXPECT_NEAR(last.fee.toDouble(), 0.60876416, 1e-9);
}

// SMA(40) is ready on the 40th close (bar index 39). The buy that close
// produces is held through the callback and fills at the open of bar 40,
// stamped with that bar's end time: the entry is never a price of the bar
// whose close produced the signal.
TEST(ReferenceBacktest, EntryFillsAtTheOpenOfTheBarAfterTheSignal)
{
  const ReferenceRun run = runReference();
  ASSERT_GT(run.bars.size(), 40u);
  const TradeRecord& first = run.result.trades().front();
  EXPECT_NEAR(first.entryPrice.toDouble(), run.bars[40].bar.open.toDouble(), 1e-9);
  EXPECT_EQ(first.entryTimeNs.raw(), run.bars[40].bar.endTime.time_since_epoch().count());
}
