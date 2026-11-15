/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/aggregator/events/bar_event.h"
#include "lrvx/engine/symbol_registry.h"
#include "lrvx/strategy/strategy.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <vector>

namespace
{

class TestStrategy : public lrvx::Strategy
{
 public:
  using lrvx::Strategy::Strategy;
  int barCallbacks = 0;

 protected:
  void onSymbolBar(lrvx::SymbolContext&, const lrvx::BarEvent&) override
  {
    ++barCallbacks;
  }
};

lrvx::BarEvent makeTimeBar(lrvx::SymbolId sym, uint64_t intervalNs, double open, double close,
                           int64_t startNs)
{
  lrvx::BarEvent ev{};
  ev.symbol = sym;
  ev.barType = lrvx::BarType::Time;
  ev.barTypeParam = intervalNs;
  ev.bar.open = lrvx::Price::fromDouble(open);
  ev.bar.close = lrvx::Price::fromDouble(close);
  ev.bar.high = lrvx::Price::fromDouble(std::max(open, close));
  ev.bar.low = lrvx::Price::fromDouble(std::min(open, close));
  ev.bar.startTime = lrvx::TimePoint{std::chrono::nanoseconds{startNs}};
  ev.bar.endTime = lrvx::TimePoint{std::chrono::nanoseconds{startNs + (int64_t)intervalNs}};
  return ev;
}

lrvx::SymbolId addInfo(lrvx::SymbolRegistry& reg, const std::string& spelling)
{
  lrvx::SymbolInfo info;
  info.exchange = "test";
  info.symbol = spelling;
  info.type = lrvx::InstrumentType::Spot;
  info.tickSize = lrvx::Price::fromDouble(0.01);
  return reg.registerSymbol(info);
}

struct Fixture
{
  std::unique_ptr<lrvx::SymbolRegistry> reg = std::make_unique<lrvx::SymbolRegistry>();
  lrvx::SymbolId btc = addInfo(*reg, "BTCUSDT");
};

}  // namespace

TEST(MultiTfAlignment, ReturnsNulloptBeforeAnyBar)
{
  Fixture f;
  TestStrategy s(1, std::vector<lrvx::SymbolId>{f.btc}, *f.reg);
  EXPECT_FALSE(s.lastClosedBar(f.btc, lrvx::BarType::Time, 60'000'000'000ull).has_value());
  EXPECT_TRUE(s.lastNClosedBars(f.btc, lrvx::BarType::Time, 60'000'000'000ull, 3).empty());
}

TEST(MultiTfAlignment, RingStoresLatestBar)
{
  Fixture f;
  TestStrategy s(1, std::vector<lrvx::SymbolId>{f.btc}, *f.reg);
  const uint64_t H1 = 3'600'000'000'000ull;
  s.onBar(makeTimeBar(f.btc, H1, 100.0, 101.0, 0));
  s.onBar(makeTimeBar(f.btc, H1, 101.0, 102.5, (int64_t)H1));
  auto last = s.lastClosedBar(f.btc, lrvx::BarType::Time, H1);
  ASSERT_TRUE(last.has_value());
  EXPECT_NEAR(last->close.toDouble(), 102.5, 1e-9);
  EXPECT_EQ(s.barCallbacks, 2);
}

TEST(MultiTfAlignment, RingHonorsCapacity)
{
  Fixture f;
  TestStrategy s(1, std::vector<lrvx::SymbolId>{f.btc}, *f.reg);
  s.setBarRingCapacity(3);
  const uint64_t M5 = 300'000'000'000ull;
  for (int i = 0; i < 7; ++i)
  {
    s.onBar(makeTimeBar(f.btc, M5, 100.0 + i, 100.5 + i, (int64_t)M5 * i));
  }
  auto last3 = s.lastNClosedBars(f.btc, lrvx::BarType::Time, M5, 3);
  ASSERT_EQ(last3.size(), 3u);
  EXPECT_NEAR(last3[0].close.toDouble(), 104.5, 1e-9);
  EXPECT_NEAR(last3[1].close.toDouble(), 105.5, 1e-9);
  EXPECT_NEAR(last3[2].close.toDouble(), 106.5, 1e-9);
}

TEST(MultiTfAlignment, MultiTfRingsStayIndependent)
{
  Fixture f;
  TestStrategy s(1, std::vector<lrvx::SymbolId>{f.btc}, *f.reg);
  const uint64_t M5 = 300'000'000'000ull;
  const uint64_t H1 = 3'600'000'000'000ull;
  s.onBar(makeTimeBar(f.btc, M5, 100.0, 100.5, 0));
  s.onBar(makeTimeBar(f.btc, H1, 100.0, 105.0, 0));
  s.onBar(makeTimeBar(f.btc, M5, 100.5, 100.8, (int64_t)M5));
  auto m5 = s.lastClosedBar(f.btc, lrvx::BarType::Time, M5);
  auto h1 = s.lastClosedBar(f.btc, lrvx::BarType::Time, H1);
  ASSERT_TRUE(m5.has_value());
  ASSERT_TRUE(h1.has_value());
  EXPECT_NEAR(m5->close.toDouble(), 100.8, 1e-9);
  EXPECT_NEAR(h1->close.toDouble(), 105.0, 1e-9);
}

TEST(MultiTfAlignment, MultiSymbolRingsStayIndependent)
{
  auto reg = std::make_unique<lrvx::SymbolRegistry>();
  lrvx::SymbolId btc = addInfo(*reg, "BTCUSDT");
  lrvx::SymbolId eth = addInfo(*reg, "ETHUSDT");
  TestStrategy s(1, std::vector<lrvx::SymbolId>{btc, eth}, *reg);
  const uint64_t H4 = 14'400'000'000'000ull;
  s.onBar(makeTimeBar(btc, H4, 50000.0, 50100.0, 0));
  s.onBar(makeTimeBar(eth, H4, 3000.0, 3010.0, 0));
  auto btcBar = s.lastClosedBar(btc, lrvx::BarType::Time, H4);
  auto ethBar = s.lastClosedBar(eth, lrvx::BarType::Time, H4);
  ASSERT_TRUE(btcBar.has_value());
  ASSERT_TRUE(ethBar.has_value());
  EXPECT_NEAR(btcBar->close.toDouble(), 50100.0, 1e-9);
  EXPECT_NEAR(ethBar->close.toDouble(), 3010.0, 1e-9);
}

namespace
{

std::vector<double> closesOf(const std::vector<lrvx::Bar>& bars)
{
  std::vector<double> out;
  for (const auto& b : bars)
  {
    out.push_back(b.close.toDouble());
  }
  return out;
}

}  // namespace

// A ring already full at the default 64 bars: lowering the capacity to 3
// leaves only the newest 3 readable at once, oldest first, and the ring stays
// at 3 after the next bar.
TEST(MultiTfAlignment, LoweringCapacityTrimsAFullRing)
{
  Fixture f;
  TestStrategy s(1, std::vector<lrvx::SymbolId>{f.btc}, *f.reg);
  const uint64_t M5 = 300'000'000'000ull;
  ASSERT_EQ(s.barRingCapacity(), 64u);
  for (int i = 0; i < 70; ++i)
  {
    s.onBar(makeTimeBar(f.btc, M5, 1000.0 + i, 1000.25 + i, (int64_t)M5 * i));
  }
  ASSERT_EQ(s.lastNClosedBars(f.btc, lrvx::BarType::Time, M5, 100).size(), 64u);

  s.setBarRingCapacity(3);
  EXPECT_EQ(s.barRingCapacity(), 3u);
  EXPECT_EQ(closesOf(s.lastNClosedBars(f.btc, lrvx::BarType::Time, M5, 100)),
            (std::vector<double>{1067.25, 1068.25, 1069.25}));
  auto last = s.lastClosedBar(f.btc, lrvx::BarType::Time, M5);
  ASSERT_TRUE(last.has_value());
  EXPECT_NEAR(last->close.toDouble(), 1069.25, 1e-9);

  s.onBar(makeTimeBar(f.btc, M5, 1070.0, 1070.25, (int64_t)M5 * 70));
  EXPECT_EQ(closesOf(s.lastNClosedBars(f.btc, lrvx::BarType::Time, M5, 100)),
            (std::vector<double>{1068.25, 1069.25, 1070.25}));
}

// Raising the capacity keeps every bar already in the ring and lets it grow.
TEST(MultiTfAlignment, RaisingCapacityKeepsExistingBars)
{
  Fixture f;
  TestStrategy s(1, std::vector<lrvx::SymbolId>{f.btc}, *f.reg);
  const uint64_t M5 = 300'000'000'000ull;
  s.setBarRingCapacity(3);
  for (int i = 0; i < 5; ++i)
  {
    s.onBar(makeTimeBar(f.btc, M5, 2000.0 + i, 2000.5 + i, (int64_t)M5 * i));
  }
  s.setBarRingCapacity(5);
  EXPECT_EQ(closesOf(s.lastNClosedBars(f.btc, lrvx::BarType::Time, M5, 100)),
            (std::vector<double>{2002.5, 2003.5, 2004.5}));
  s.onBar(makeTimeBar(f.btc, M5, 2005.0, 2005.5, (int64_t)M5 * 5));
  s.onBar(makeTimeBar(f.btc, M5, 2006.0, 2006.5, (int64_t)M5 * 6));
  EXPECT_EQ(closesOf(s.lastNClosedBars(f.btc, lrvx::BarType::Time, M5, 100)),
            (std::vector<double>{2002.5, 2003.5, 2004.5, 2005.5, 2006.5}));
  s.onBar(makeTimeBar(f.btc, M5, 2007.0, 2007.5, (int64_t)M5 * 7));
  EXPECT_EQ(closesOf(s.lastNClosedBars(f.btc, lrvx::BarType::Time, M5, 100)),
            (std::vector<double>{2003.5, 2004.5, 2005.5, 2006.5, 2007.5}));
}
