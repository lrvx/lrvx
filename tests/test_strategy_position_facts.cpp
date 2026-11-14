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
#include "lrvx/position/abstract_position_manager.h"
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

// Scenario B: an add after a partial close, then a short that is added to,
// partly closed and added to again. Twenty bars, same prices as scenario A.
//
//   bar 1  buy 1   -> 120 at bar 2   long 1, entry 120
//   bar 3  buy 1   -> 140 at bar 4   long 2, entry 130
//   bar 5  sell 1  -> 160 at bar 6   long 1, entry 130 (the manager's FIFO basis: 140)
//   bar 7  buy 1   -> 180 at bar 8   long 2, entry (130 + 180) / 2 = 155 (FIFO: 160)
//   bar 9  sell 4  -> 200 at bar 10  short 2, entry 200, count restarts
//   bar 11 sell 2  -> 220 at bar 12  short 4, entry 210
//   bar 13 buy 1   -> 240 at bar 14  short 3, entry 210 (FIFO: 213.33)
//   bar 15 sell 1  -> 260 at bar 16  short 4, entry (3 * 210 + 260) / 4 = 222.5 (FIFO: 225)
//   bar 17 buy 4   -> 280 at bar 18  flat
namespace
{

const std::vector<std::pair<size_t, double>> kPlanB = {
    {1, 1.0}, {3, 1.0}, {5, -1.0}, {7, 1.0}, {9, -4.0}, {11, -2.0}, {13, 1.0}, {15, -1.0}, {17, 4.0}};

const std::vector<Expect> kExpectedB = {
    {std::nullopt, 0},
    {std::nullopt, 0},
    {120.0, 1},
    {120.0, 2},
    {130.0, 3},
    {130.0, 4},
    {130.0, 5},
    {130.0, 6},
    {155.0, 7},
    {155.0, 8},
    {200.0, 1},
    {200.0, 2},
    {210.0, 3},
    {210.0, 4},
    {210.0, 5},
    {210.0, 6},
    {222.5, 7},
    {222.5, 8},
    {std::nullopt, 0},
    {std::nullopt, 0},
};

const double kPositionsB[] = {0, 0, 1, 1, 2, 2, 1, 1, 2, 2, -2, -2, -4, -4, -3, -3, -4, -4, 0, 0};

struct FillSeen
{
  double fillPrice;
  Seen ctx;
  std::optional<double> managerAvg;
};

// Scenario strategy that also records what each fill callback reads.
class FillRecordingStrategy : public ScenarioStrategy
{
 public:
  using ScenarioStrategy::ScenarioStrategy;

  std::vector<FillSeen> fills;
  std::vector<std::optional<double>> managerAvgOnBar;

 protected:
  void onSymbolBar(SymbolContext& ctx, const BarEvent& ev) override
  {
    managerAvgOnBar.push_back(ctx.avgEntryPrice ? std::optional<double>(ctx.avgEntryPrice->toDouble())
                                                : std::nullopt);
    ScenarioStrategy::onSymbolBar(ctx, ev);
  }

  void onSymbolFill(SymbolContext& ctx, const OrderEvent& ev) override
  {
    fills.push_back(FillSeen{ev.fillPrice.toDouble(),
                             Seen{entryOf(ctx), ctx.barsInPosition, ctx.position.toDouble()},
                             ctx.avgEntryPrice ? std::optional<double>(ctx.avgEntryPrice->toDouble())
                                               : std::nullopt});
  }
};

}  // namespace

TEST(StrategyPositionFacts, AddsAfterAPartialCloseAndOnAShortAreWeightedByTheirFills)
{
  SymbolRegistry reg;
  const SymbolId sym = addSymbol(reg, "BTCUSDT");
  FillRecordingStrategy strat({sym}, reg);
  strat.plan = {kPlanB};

  BacktestConfig cfg;
  cfg.feeRate = 0.0;
  BacktestRunner runner(cfg);
  runner.setStrategy(&strat);
  runner.runBars(scenarioBars(sym, 20));

  ASSERT_EQ(strat.seen[0].size(), kExpectedB.size());
  for (size_t i = 0; i < kExpectedB.size(); ++i)
  {
    ASSERT_DOUBLE_EQ(strat.seen[0][i].position, kPositionsB[i]) << "bar " << i << " (premise)";
  }
  // The premise that makes this scenario worth running: after the partial
  // closes the position manager's cost basis is a FIFO figure, different
  // from the weighted entry, so an entry taken from it would show.
  ASSERT_TRUE(strat.managerAvgOnBar[8].has_value());
  ASSERT_NEAR(*strat.managerAvgOnBar[8], 160.0, 1e-9) << "premise: FIFO basis after the re-add";
  ASSERT_TRUE(strat.managerAvgOnBar[16].has_value());
  ASSERT_NEAR(*strat.managerAvgOnBar[16], 225.0, 1e-9) << "premise: FIFO basis after the short re-add";

  for (size_t i = 0; i < kExpectedB.size(); ++i)
  {
    const auto& want = kExpectedB[i];
    const auto& got = strat.seen[0][i];
    EXPECT_EQ(got.entry.has_value(), want.entry.has_value()) << "bar " << i;
    if (got.entry && want.entry)
    {
      EXPECT_NEAR(*got.entry, *want.entry, 1e-9) << "bar " << i;
    }
    EXPECT_EQ(got.bars, want.bars) << "bar " << i << ": barsInPosition";
  }
}

TEST(StrategyPositionFacts, FillCallbacksReadTheFactsTheFillLeaves)
{
  SymbolRegistry reg;
  const SymbolId sym = addSymbol(reg, "BTCUSDT");
  FillRecordingStrategy strat({sym}, reg);
  strat.plan = {kPlanB};

  BacktestConfig cfg;
  cfg.feeRate = 0.0;
  BacktestRunner runner(cfg);
  runner.setStrategy(&strat);
  runner.runBars(scenarioBars(sym, 20));

  // (fill price, position, entry, bars). The fill that opens or flips a
  // position reads 0 bars: no bar has been seen holding it yet. The fill
  // that flattens reads no entry and 0 bars, not the count of the position
  // it closed.
  struct Want
  {
    double price;
    double position;
    std::optional<double> entry;
    uint32_t bars;
  };
  const std::vector<Want> want = {
      {120, 1, 120.0, 0},
      {140, 2, 130.0, 2},
      {160, 1, 130.0, 4},
      {180, 2, 155.0, 6},
      {200, -2, 200.0, 0},
      {220, -4, 210.0, 2},
      {240, -3, 210.0, 4},
      {260, -4, 222.5, 6},
      {280, 0, std::nullopt, 0},
  };
  ASSERT_EQ(strat.fills.size(), want.size());
  for (size_t i = 0; i < want.size(); ++i)
  {
    const FillSeen& got = strat.fills[i];
    ASSERT_DOUBLE_EQ(got.fillPrice, want[i].price) << "fill " << i << " (premise)";
    ASSERT_DOUBLE_EQ(got.ctx.position, want[i].position) << "fill " << i << " (premise)";
    EXPECT_EQ(got.ctx.entry.has_value(), want[i].entry.has_value()) << "fill " << i;
    if (got.ctx.entry && want[i].entry)
    {
      EXPECT_NEAR(*got.ctx.entry, *want[i].entry, 1e-9) << "fill " << i;
    }
    EXPECT_EQ(got.ctx.bars, want[i].bars) << "fill " << i << ": barsInPosition";
  }
}

namespace
{

// A position manager fed from outside the strategy: it can report a
// position before it knows what was paid for it, as one synced from a venue
// whose cost basis arrives later does.
class ExternalPositions : public IPositionManager
{
 public:
  using IPositionManager::IPositionManager;
  Quantity position{};
  std::optional<Price> avg;
  Quantity getPosition(SymbolId) const override { return position; }
  std::optional<Price> getAverageEntryPrice(SymbolId) const override { return avg; }
  PositionSnapshot positionSnapshot(SymbolId) const override { return {position, avg}; }
};

class TradeReader : public Strategy
{
 public:
  TradeReader(SymbolId sym, SymbolRegistry& reg) : Strategy(1, std::vector<SymbolId>{sym}, reg) {}
  std::vector<Seen> seen;

 protected:
  void onSymbolTrade(SymbolContext& ctx, const TradeEvent&) override
  {
    seen.push_back(Seen{entryOf(ctx), ctx.barsInPosition, ctx.position.toDouble()});
  }
};

TradeEvent tradeOf(SymbolId sym, double price, int64_t ns)
{
  TradeEvent ev{};
  ev.trade.symbol = sym;
  ev.trade.price = Price::fromDouble(price);
  ev.trade.quantity = Quantity::fromDouble(1.0);
  ev.trade.isBuy = true;
  ev.trade.exchangeTsNs = UnixNanos{ns};
  return ev;
}

}  // namespace

TEST(StrategyPositionFacts, EntryAppearsWhenTheManagerLearnsTheCostBasisLate)
{
  SymbolRegistry reg;
  const SymbolId sym = addSymbol(reg, "BTCUSDT");
  ExternalPositions positions{2};
  TradeReader strat(sym, reg);
  strat.setPositionManager(&positions);

  strat.onTrade(tradeOf(sym, 100.0, 1));
  positions.position = Quantity::fromDouble(2.0);
  strat.onTrade(tradeOf(sym, 101.0, 2));
  // The basis becomes known while the position stays the same.
  positions.avg = Price::fromDouble(150.0);
  strat.onTrade(tradeOf(sym, 102.0, 3));
  strat.onTrade(tradeOf(sym, 103.0, 4));

  ASSERT_EQ(strat.seen.size(), 4u);
  EXPECT_FALSE(strat.seen[0].entry.has_value()) << "flat";
  ASSERT_DOUBLE_EQ(strat.seen[1].position, 2.0) << "premise";
  EXPECT_FALSE(strat.seen[1].entry.has_value()) << "long 2, but nothing says at what price yet";
  for (size_t i : {2u, 3u})
  {
    ASSERT_DOUBLE_EQ(strat.seen[i].position, 2.0) << "premise";
    ASSERT_TRUE(strat.seen[i].entry.has_value()) << "trade " << i << ": the basis is known now";
    EXPECT_NEAR(*strat.seen[i].entry, 150.0, 1e-9) << "trade " << i;
  }
}

namespace
{

OrderEvent fillOf(SymbolId sym, double price, double qty)
{
  OrderEvent ev{};
  ev.status = OrderEventStatus::FILLED;
  ev.order.symbol = sym;
  ev.fillPrice = Price::fromDouble(price);
  ev.fillQty = Quantity::fromDouble(qty);
  return ev;
}

}  // namespace

TEST(StrategyPositionFacts, AnAddToAPositionOfUnknownCostHasNoEntryToWeight)
{
  SymbolRegistry reg;
  const SymbolId sym = addSymbol(reg, "BTCUSDT");
  // A manager that keeps no cost basis at all.
  ExternalPositions positions{2};
  TradeReader strat(sym, reg);
  strat.setPositionManager(&positions);

  // Long 2 that the strategy did not fill itself: nothing says at what price.
  positions.position = Quantity::fromDouble(2.0);
  strat.onTrade(tradeOf(sym, 100.0, 1));
  // A buy of 1 at 130 fills on top of it.
  positions.position = Quantity::fromDouble(3.0);
  strat.onOrderEvent(fillOf(sym, 130.0, 1.0));
  strat.onTrade(tradeOf(sym, 131.0, 2));
  // Flat, then a buy of 1 at 140 from flat.
  positions.position = Quantity{};
  strat.onOrderEvent(fillOf(sym, 120.0, 3.0));
  positions.position = Quantity::fromDouble(1.0);
  strat.onOrderEvent(fillOf(sym, 140.0, 1.0));
  strat.onTrade(tradeOf(sym, 141.0, 3));

  ASSERT_EQ(strat.seen.size(), 3u);
  ASSERT_DOUBLE_EQ(strat.seen[0].position, 2.0) << "premise";
  EXPECT_FALSE(strat.seen[0].entry.has_value()) << "long 2 at an unknown price";
  ASSERT_DOUBLE_EQ(strat.seen[1].position, 3.0) << "premise";
  EXPECT_FALSE(strat.seen[1].entry.has_value())
      << "two lots at an unknown price and one at 130 have no known average";
  ASSERT_DOUBLE_EQ(strat.seen[2].position, 1.0) << "premise";
  ASSERT_TRUE(strat.seen[2].entry.has_value()) << "a position opened by a fill has its price";
  EXPECT_NEAR(*strat.seen[2].entry, 140.0, 1e-9);
}
