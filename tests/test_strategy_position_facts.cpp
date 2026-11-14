/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// SymbolContext::entryPrice and SymbolContext::barsInPosition: the average
// entry price of the open position and the number of bar callbacks the
// strategy received while holding it.
//
// The scenario is the one python/tests/test_position_facts.py, the Node,
// QuickJS and Codon tests run: twelve bars with open 100 + 10 i, high
// open + 3, low open - 2, close open + 1, so a fill at the open, high, low or
// close of any bar gives a different number. An order sent from onBar fills
// at the next bar's open, before that bar's onBar.
//
//   bar 1 buy 1   -> fills 120 at bar 2   long 1, entry 120
//   bar 3 buy 1   -> fills 140 at bar 4   long 2, entry 130 (weighted)
//   bar 5 sell 1  -> fills 160 at bar 6   long 1, entry 130 (FIFO would say 140)
//   bar 7 sell 3  -> fills 180 at bar 8   short 2, entry 180, count restarts
//   bar 9 buy 2   -> fills 200 at bar 10  flat

#include "lrvx/aggregator/events/bar_event.h"
#include "lrvx/backtest/backtest_runner.h"
#include "lrvx/book/events/trade_event.h"
#include "lrvx/engine/symbol_registry.h"
#include "lrvx/replay/binary_format_v1.h"
#include "lrvx/replay/writers/binary_log_writer.h"
#include "lrvx/strategy/strategy.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <vector>

using namespace lrvx;

namespace
{

constexpr int64_t kMinuteNs = 60'000'000'000LL;

struct Seen
{
  std::optional<double> entry;
  uint32_t bars;
  double position;
};

std::optional<double> entryOf(const SymbolContext& ctx)
{
  if (!ctx.entryPrice)
  {
    return std::nullopt;
  }
  return ctx.entryPrice->toDouble();
}

SymbolId addSymbol(SymbolRegistry& reg, const std::string& name)
{
  SymbolInfo info;
  info.exchange = "test";
  info.symbol = name;
  info.type = InstrumentType::Spot;
  info.tickSize = Price::fromDouble(0.01);
  return reg.registerSymbol(info);
}

BarEvent makeBar(SymbolId sym, size_t i, double base = 100.0)
{
  const double open = base + 10.0 * static_cast<double>(i);
  BarEvent ev{};
  ev.symbol = sym;
  ev.barType = BarType::Time;
  ev.barTypeParam = static_cast<uint64_t>(kMinuteNs);
  ev.bar.open = Price::fromDouble(open);
  ev.bar.high = Price::fromDouble(open + 3.0);
  ev.bar.low = Price::fromDouble(open - 2.0);
  ev.bar.close = Price::fromDouble(open + 1.0);
  ev.bar.volume = Volume::fromDouble(100.0);
  const int64_t ts = static_cast<int64_t>(i) * kMinuteNs;
  ev.bar.startTime = TimePoint{std::chrono::nanoseconds{ts}};
  ev.bar.endTime = TimePoint{std::chrono::nanoseconds{ts + kMinuteNs}};
  ev.bar.reason = BarCloseReason::Threshold;
  return ev;
}

// Sends the scenario's orders for one symbol and records what each bar saw.
class ScenarioStrategy : public Strategy
{
 public:
  ScenarioStrategy(std::vector<SymbolId> syms, SymbolRegistry& reg)
      : Strategy(1, syms, reg), _syms(std::move(syms))
  {
    seen.resize(_syms.size());
    barIndex.resize(_syms.size(), 0);
  }

  // Per symbol slot: bar index -> (buy > 0 / sell < 0) quantity.
  std::vector<std::vector<std::pair<size_t, double>>> plan;
  std::vector<std::vector<Seen>> seen;
  std::vector<size_t> barIndex;

 protected:
  void onSymbolBar(SymbolContext& ctx, const BarEvent& ev) override
  {
    size_t slot = 0;
    while (slot < _syms.size() && _syms[slot] != ev.symbol)
    {
      ++slot;
    }
    ASSERT_LT(slot, _syms.size());
    seen[slot].push_back(Seen{entryOf(ctx), ctx.barsInPosition, ctx.position.toDouble()});
    const size_t i = barIndex[slot]++;
    if (slot < plan.size())
    {
      for (const auto& [at, qty] : plan[slot])
      {
        if (at == i)
        {
          if (qty > 0)
          {
            emitMarketBuy(ev.symbol, Quantity::fromDouble(qty));
          }
          else
          {
            emitMarketSell(ev.symbol, Quantity::fromDouble(-qty));
          }
        }
      }
    }
  }

 private:
  std::vector<SymbolId> _syms;
};

const std::vector<std::pair<size_t, double>> kPlan = {
    {1, 1.0}, {3, 1.0}, {5, -1.0}, {7, -3.0}, {9, 2.0}};

struct Expect
{
  std::optional<double> entry;
  uint32_t bars;
};

const std::vector<Expect> kExpected = {
    {std::nullopt, 0},
    {std::nullopt, 0},
    {120.0, 1},
    {120.0, 2},
    {130.0, 3},
    {130.0, 4},
    {130.0, 5},
    {130.0, 6},
    {180.0, 1},
    {180.0, 2},
    {std::nullopt, 0},
    {std::nullopt, 0},
};

void expectScenario(const std::vector<Seen>& seen, const char* label)
{
  ASSERT_EQ(seen.size(), kExpected.size()) << label;
  for (size_t i = 0; i < kExpected.size(); ++i)
  {
    const auto& want = kExpected[i];
    const auto& got = seen[i];
    EXPECT_EQ(got.entry.has_value(), want.entry.has_value())
        << label << " bar " << i << ": entryPrice presence (position " << got.position << ")";
    if (got.entry && want.entry)
    {
      EXPECT_NEAR(*got.entry, *want.entry, 1e-9) << label << " bar " << i;
    }
    EXPECT_EQ(got.bars, want.bars) << label << " bar " << i << ": barsInPosition";
  }
}

std::vector<BarEvent> scenarioBars(SymbolId sym, size_t n = 12)
{
  std::vector<BarEvent> bars;
  for (size_t i = 0; i < n; ++i)
  {
    bars.push_back(makeBar(sym, i));
  }
  return bars;
}

}  // namespace

TEST(StrategyPositionFacts, FreshContextHasNoEntryAndNoBars)
{
  SymbolContext ctx;
  EXPECT_FALSE(ctx.entryPrice.has_value());
  EXPECT_EQ(ctx.barsInPosition, 0u);
  ctx.entryPrice = Price::fromDouble(5.0);
  ctx.barsInPosition = 7;
  ctx.reset();
  EXPECT_FALSE(ctx.entryPrice.has_value()) << "reset() clears the entry price";
  EXPECT_EQ(ctx.barsInPosition, 0u) << "reset() clears the bar count";
}

TEST(StrategyPositionFacts, RunBarsWeightsAddsKeepsPartialClosesAndRestartsOnFlip)
{
  SymbolRegistry reg;
  const SymbolId sym = addSymbol(reg, "BTCUSDT");
  ScenarioStrategy strat({sym}, reg);
  strat.plan = {kPlan};

  BacktestConfig cfg;
  cfg.feeRate = 0.0;
  BacktestRunner runner(cfg);
  runner.setStrategy(&strat);
  runner.runBars(scenarioBars(sym));

  // The premise: the fills land where the scenario says.
  ASSERT_EQ(strat.seen[0].size(), 12u);
  const double positions[] = {0, 0, 1, 1, 2, 2, 1, 1, -2, -2, 0, 0};
  for (size_t i = 0; i < 12; ++i)
  {
    ASSERT_DOUBLE_EQ(strat.seen[0][i].position, positions[i]) << "bar " << i;
  }
  expectScenario(strat.seen[0], "run_bars");
}

TEST(StrategyPositionFacts, FeesDoNotMoveTheEntryPrice)
{
  SymbolRegistry reg;
  const SymbolId sym = addSymbol(reg, "BTCUSDT");
  ScenarioStrategy strat({sym}, reg);
  strat.plan = {kPlan};

  BacktestConfig cfg;
  cfg.feeRate = 0.01;
  BacktestRunner runner(cfg);
  runner.setStrategy(&strat);
  runner.runBars(scenarioBars(sym));
  expectScenario(strat.seen[0], "fee 1%");
}

TEST(StrategyPositionFacts, ASecondRunOnTheSameRunnerStartsClean)
{
  SymbolRegistry reg;
  const SymbolId sym = addSymbol(reg, "BTCUSDT");
  ScenarioStrategy strat({sym}, reg);
  strat.plan = {kPlan};

  BacktestConfig cfg;
  cfg.feeRate = 0.0;
  BacktestRunner runner(cfg);
  runner.setStrategy(&strat);

  // The first run stops on bar 3: long 1 at 120, two bars held.
  runner.runBars(scenarioBars(sym, 4));
  ASSERT_EQ(strat.seen[0].size(), 4u);
  ASSERT_TRUE(strat.seen[0][3].entry.has_value());
  EXPECT_NEAR(*strat.seen[0][3].entry, 120.0, 1e-9);
  EXPECT_EQ(strat.seen[0][3].bars, 2u);

  strat.seen[0].clear();
  strat.barIndex[0] = 0;
  runner.runBars(scenarioBars(sym));
  expectScenario(strat.seen[0], "second run");
}

TEST(StrategyPositionFacts, SymbolsAreCountedIndependently)
{
  SymbolRegistry reg;
  const SymbolId a = addSymbol(reg, "AAA");
  const SymbolId b = addSymbol(reg, "BBB");
  ScenarioStrategy strat({a, b}, reg);
  // A runs the scenario; B sells 1 on its bar 4 and holds.
  strat.plan = {kPlan, {{4, -1.0}}};

  BacktestConfig cfg;
  cfg.feeRate = 0.0;
  BacktestRunner runner(cfg);
  runner.setStrategy(&strat);

  std::vector<BarEvent> bars;
  for (size_t i = 0; i < 12; ++i)
  {
    bars.push_back(makeBar(a, i));
    bars.push_back(makeBar(b, i, 1000.0));
  }
  runner.runBars(bars);

  expectScenario(strat.seen[0], "symbol A");
  ASSERT_EQ(strat.seen[1].size(), 12u);
  for (size_t i = 0; i < 12; ++i)
  {
    const Seen& s = strat.seen[1][i];
    if (i <= 4)
    {
      EXPECT_FALSE(s.entry.has_value()) << "B bar " << i << " is flat";
      EXPECT_EQ(s.bars, 0u) << "B bar " << i;
    }
    else
    {
      ASSERT_TRUE(s.entry.has_value()) << "B bar " << i;
      EXPECT_NEAR(*s.entry, 1050.0, 1e-9) << "B sold at its bar 5 open";
      EXPECT_EQ(s.bars, static_cast<uint32_t>(i - 4)) << "B bar " << i;
    }
  }
}

namespace
{

class TradeProbe : public Strategy
{
 public:
  TradeProbe(SymbolId sym, SymbolRegistry& reg) : Strategy(1, std::vector<SymbolId>{sym}, reg), sym(sym)
  {
  }

  SymbolId sym;
  std::vector<Seen> seen;
  std::vector<std::pair<double, double>> fills;  // (price, qty)

 protected:
  void onSymbolFill(SymbolContext&, const OrderEvent& ev) override
  {
    fills.emplace_back(ev.fillPrice.toDouble(), ev.fillQty.toDouble());
  }
  void onSymbolTrade(SymbolContext& ctx, const TradeEvent&) override
  {
    seen.push_back(Seen{entryOf(ctx), ctx.barsInPosition, ctx.position.toDouble()});
    if (seen.size() == 1)
    {
      emitMarketBuy(sym, Quantity::fromDouble(1.0));
    }
    else if (seen.size() == 3)
    {
      emitMarketBuy(sym, Quantity::fromDouble(3.0));
    }
  }
};

}  // namespace

TEST(StrategyPositionFacts, TickReplaySetsTheEntryButCountsNoBars)
{
  SymbolRegistry reg;
  const SymbolId sym = addSymbol(reg, "BTCUSDT");
  ASSERT_EQ(sym, 1u);

  const auto dir = std::filesystem::temp_directory_path() /
                   ("lrvx_position_facts_" + std::to_string(std::random_device{}()));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  {
    replay::WriterConfig wc;
    wc.output_dir = dir;
    replay::BinaryLogWriter writer(wc);
    const double prices[] = {100.0, 104.0, 108.0, 112.0, 116.0, 120.0};
    uint64_t id = 0;
    for (double p : prices)
    {
      replay::TradeRecord r{};
      r.exchange_ts_ns = static_cast<int64_t>(id + 1) * 1'000'000'000LL;
      r.recv_ts_ns = r.exchange_ts_ns;
      r.price_raw = Price::fromDouble(p).raw();
      r.qty_raw = Quantity::fromDouble(10.0).raw();
      r.symbol_id = sym;
      r.side = 1;
      r.trade_id = ++id;
      writer.writeTrade(r);
    }
    writer.close();
  }

  TradeProbe strat(sym, reg);
  BacktestConfig cfg;
  cfg.feeRate = 0.0;
  BacktestRunner runner(cfg);
  runner.setStrategy(&strat);
  runner.runTape(dir);
  std::filesystem::remove_all(dir);

  ASSERT_EQ(strat.seen.size(), 6u);
  EXPECT_FALSE(strat.seen[0].entry.has_value());
  for (size_t i = 1; i < strat.seen.size(); ++i)
  {
    EXPECT_EQ(strat.seen[i].bars, 0u) << "no bars on a tick replay (trade " << i << ")";
    if (strat.seen[i].position != 0.0)
    {
      ASSERT_TRUE(strat.seen[i].entry.has_value()) << "trade " << i;
      EXPECT_GE(*strat.seen[i].entry, 100.0) << "trade " << i;
      EXPECT_LE(*strat.seen[i].entry, 120.0) << "trade " << i;
    }
  }
  ASSERT_DOUBLE_EQ(strat.seen.back().position, 4.0);
  ASSERT_EQ(strat.fills.size(), 2u);
  const auto [p1, q1] = strat.fills[0];
  const auto [p2, q2] = strat.fills[1];
  ASSERT_NE(p1, p2) << "the premise: the two buys fill at different prices";
  ASSERT_TRUE(strat.seen.back().entry.has_value());
  EXPECT_NEAR(*strat.seen.back().entry, (p1 * q1 + p2 * q2) / (q1 + q2), 1e-9)
      << "the entry is the quantity-weighted mean of the fills";
  for (const Seen& s : strat.seen)
  {
    if (s.position == 1.0)
    {
      ASSERT_TRUE(s.entry.has_value());
      EXPECT_NEAR(*s.entry, p1, 1e-9) << "one fill: the entry is its price";
    }
  }
}
