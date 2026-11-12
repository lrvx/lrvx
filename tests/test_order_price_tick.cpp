/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Order prices on the tick. A venue refuses a limit price, trigger price or
// trailing distance that is not a whole number of ticks, so the simulator
// refuses it too; prices the simulator computes itself (a slipped fill, a
// percentage trailing trigger) are snapped onto the tick against the owner.
// The symbol's tick is 0.1 throughout unless a test says otherwise.

#include "lrvx/aggregator/events/bar_event.h"
#include "lrvx/backtest/backtest_config.h"
#include "lrvx/backtest/backtest_runner.h"
#include "lrvx/backtest/simulated_clock.h"
#include "lrvx/backtest/simulated_executor.h"
#include "lrvx/engine/symbol_registry.h"
#include "lrvx/execution/order_price_rule.h"
#include "lrvx/strategy/strategy.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <memory_resource>
#include <string>
#include <vector>

using namespace lrvx;

namespace
{

constexpr int64_t kMinuteNs = 60'000'000'000LL;

SymbolId addSymbol(SymbolRegistry& reg, const std::string& name, double tick, bool known = true)
{
  SymbolInfo info;
  info.exchange = "test";
  info.symbol = name;
  info.tickSize = Price::fromDouble(tick);
  info.tickSizeKnown = known;
  return reg.registerSymbol(info);
}

SymbolId addSymbolWithoutTick(SymbolRegistry& reg, const std::string& name)
{
  SymbolInfo info;
  info.exchange = "test";
  info.symbol = name;
  return reg.registerSymbol(info);
}

Order limitOrder(OrderId id, SymbolId sym, Side side, double price, double qty = 1.0)
{
  Order o;
  o.id = id;
  o.symbol = sym;
  o.side = side;
  o.type = OrderType::LIMIT;
  o.price = Price::fromDouble(price);
  o.quantity = Quantity::fromDouble(qty);
  return o;
}

Order marketOrder(OrderId id, SymbolId sym, Side side, double qty = 1.0)
{
  Order o;
  o.id = id;
  o.symbol = sym;
  o.side = side;
  o.type = OrderType::MARKET;
  o.quantity = Quantity::fromDouble(qty);
  return o;
}

Order conditional(OrderId id, SymbolId sym, Side side, OrderType type, double trigger,
                  double limit = 0.0)
{
  Order o;
  o.id = id;
  o.symbol = sym;
  o.side = side;
  o.type = type;
  o.triggerPrice = Price::fromDouble(trigger);
  o.price = Price::fromDouble(limit);
  o.quantity = Quantity::fromDouble(1.0);
  return o;
}

Order trailing(OrderId id, SymbolId sym, Side side, double offset, int32_t bps = 0)
{
  Order o;
  o.id = id;
  o.symbol = sym;
  o.side = side;
  o.type = OrderType::TRAILING_STOP;
  o.trailingOffset = Price::fromDouble(offset);
  o.trailingCallbackRate = bps;
  o.quantity = Quantity::fromDouble(1.0);
  return o;
}

void pushBook(SimulatedExecutor& exec, SymbolId sym,
              const std::vector<std::pair<double, double>>& bids,
              const std::vector<std::pair<double, double>>& asks)
{
  std::pmr::monotonic_buffer_resource pool(1024);
  std::pmr::vector<BookLevel> b(&pool);
  std::pmr::vector<BookLevel> a(&pool);
  for (const auto& [px, qty] : bids)
  {
    b.emplace_back(Price::fromDouble(px), Quantity::fromDouble(qty));
  }
  for (const auto& [px, qty] : asks)
  {
    a.emplace_back(Price::fromDouble(px), Quantity::fromDouble(qty));
  }
  exec.onBookUpdate(sym, b, a);
}

// An executor wired to a registry, recording every event.
struct Harness
{
  SymbolRegistry reg;
  SimulatedClock clock;
  SimulatedExecutor exec{clock};
  std::vector<OrderEvent> events;
  SymbolId sym{0};

  explicit Harness(double tick = 0.1, bool known = true)
  {
    sym = addSymbol(reg, "BTCUSDT", tick, known);
    exec.setSymbolRegistry(&reg);
    exec.setOrderEventCallback([this](const OrderEvent& ev)
                               { events.push_back(ev); });
  }

  const OrderEvent* find(OrderId id, OrderEventStatus status) const
  {
    for (const auto& ev : events)
    {
      if (ev.order.id == id && ev.status == status)
      {
        return &ev;
      }
    }
    return nullptr;
  }

  std::vector<Fill> fillsOf(OrderId id) const
  {
    std::vector<Fill> out;
    for (const auto& f : exec.fills())
    {
      if (f.orderId == id)
      {
        out.push_back(f);
      }
    }
    return out;
  }
};

void expectRejected(const Harness& h, OrderId id, const char* reason)
{
  const OrderEvent* ev = h.find(id, OrderEventStatus::REJECTED);
  ASSERT_NE(ev, nullptr) << "order " << id << " was not rejected";
  EXPECT_EQ(ev->rejectReason, reason);
  EXPECT_EQ(h.find(id, OrderEventStatus::SUBMITTED), nullptr)
      << "a refused order never reaches the book";
  EXPECT_TRUE(h.fillsOf(id).empty());
}

void expectAccepted(const Harness& h, OrderId id)
{
  EXPECT_EQ(h.find(id, OrderEventStatus::REJECTED), nullptr) << "order " << id << " rejected";
  EXPECT_NE(h.find(id, OrderEventStatus::SUBMITTED), nullptr);
}

}  // namespace

// ---------------------------------------------------------------------------
// The one function
// ---------------------------------------------------------------------------

TEST(OrderPriceRule, SnapMovesABuyUpAndASellDown)
{
  const int64_t tick = Price::fromDouble(0.1).raw();
  const int64_t px = Price::fromDouble(100.07).raw();
  EXPECT_EQ(snapAgainstOwner(px, tick, Side::BUY), Price::fromDouble(100.1).raw());
  EXPECT_EQ(snapAgainstOwner(px, tick, Side::SELL), Price::fromDouble(100.0).raw());
}

TEST(OrderPriceRule, SnapLeavesAnOnTickPriceAlone)
{
  const int64_t tick = Price::fromDouble(0.1).raw();
  const int64_t px = Price::fromDouble(100.3).raw();
  EXPECT_EQ(snapAgainstOwner(px, tick, Side::BUY), px);
  EXPECT_EQ(snapAgainstOwner(px, tick, Side::SELL), px);
}

TEST(OrderPriceRule, SnapOfANegativePriceStillMovesAgainstTheOwner)
{
  // Spreads and some futures trade below zero.
  const int64_t tick = Price::fromDouble(0.1).raw();
  const int64_t px = Price::fromDouble(-1.05).raw();
  EXPECT_EQ(snapAgainstOwner(px, tick, Side::BUY), Price::fromDouble(-1.0).raw());
  EXPECT_EQ(snapAgainstOwner(px, tick, Side::SELL), Price::fromDouble(-1.1).raw());
}

TEST(OrderPriceRule, ZeroTickSnapsNothingAndRejectsNothing)
{
  const int64_t px = Price::fromDouble(100.07).raw();
  EXPECT_EQ(snapAgainstOwner(px, 0, Side::BUY), px);
  Order o;
  o.type = OrderType::LIMIT;
  o.price = Price::fromDouble(100.07);
  EXPECT_EQ(offTickReason(o, 0), nullptr);
}

TEST(OrderPriceRule, DefaultTickIsNotAVenueTick)
{
  SymbolInfo defaulted;
  EXPECT_EQ(orderTickRaw(defaulted), 0);

  SymbolInfo declared;
  declared.tickSizeKnown = true;
  EXPECT_EQ(orderTickRaw(declared), Price::fromDouble(0.01).raw());

  SymbolInfo other;
  other.tickSize = Price::fromDouble(0.5);
  EXPECT_EQ(orderTickRaw(other), Price::fromDouble(0.5).raw());

  SymbolInfo zero;
  zero.tickSize = Price{};
  zero.tickSizeKnown = true;
  EXPECT_EQ(orderTickRaw(zero), 0);
}

// ---------------------------------------------------------------------------
// Submit: every priced order type
// ---------------------------------------------------------------------------

TEST(OrderPriceTick, OffTickLimitIsRejected)
{
  Harness h;
  h.exec.submitOrder(limitOrder(1, h.sym, Side::BUY, 99.95));
  expectRejected(h, 1, "price_off_tick");
}

TEST(OrderPriceTick, OffTickStopTriggerIsRejected)
{
  Harness h;
  h.exec.submitOrder(conditional(1, h.sym, Side::SELL, OrderType::STOP_MARKET, 99.95));
  expectRejected(h, 1, "trigger_off_tick");
}

TEST(OrderPriceTick, OffTickTakeProfitTriggerIsRejected)
{
  Harness h;
  h.exec.submitOrder(
      conditional(1, h.sym, Side::SELL, OrderType::TAKE_PROFIT_MARKET, 100.8015));
  expectRejected(h, 1, "trigger_off_tick");
}

TEST(OrderPriceTick, ConditionalLimitChecksTriggerThenPrice)
{
  Harness h;
  h.exec.submitOrder(
      conditional(1, h.sym, Side::SELL, OrderType::TAKE_PROFIT_LIMIT, 100.8, 100.75));
  expectRejected(h, 1, "price_off_tick");
  h.exec.submitOrder(conditional(2, h.sym, Side::SELL, OrderType::STOP_LIMIT, 99.95, 99.9));
  expectRejected(h, 2, "trigger_off_tick");
}

TEST(OrderPriceTick, OffTickTrailingOffsetIsRejected)
{
  Harness h;
  h.exec.submitOrder(trailing(1, h.sym, Side::SELL, 0.25));
  expectRejected(h, 1, "trailing_offset_off_tick");
}

TEST(OrderPriceTick, OnTickOrdersOfEveryTypeAreAccepted)
{
  Harness h;
  h.exec.onTrade(h.sym, Price::fromDouble(100.0), false);
  h.exec.submitOrder(limitOrder(1, h.sym, Side::BUY, 99.9));
  h.exec.submitOrder(conditional(2, h.sym, Side::SELL, OrderType::STOP_MARKET, 99.5));
  h.exec.submitOrder(conditional(3, h.sym, Side::SELL, OrderType::TAKE_PROFIT_MARKET, 100.8));
  h.exec.submitOrder(
      conditional(4, h.sym, Side::SELL, OrderType::TAKE_PROFIT_LIMIT, 100.8, 100.7));
  h.exec.submitOrder(trailing(5, h.sym, Side::SELL, 0.3));
  h.exec.submitOrder(marketOrder(6, h.sym, Side::BUY));
  for (OrderId id = 1; id <= 6; ++id)
  {
    expectAccepted(h, id);
  }
}

TEST(OrderPriceTick, MarketOrderCarriesNoPriceToCheck)
{
  Harness h;
  Order m = marketOrder(1, h.sym, Side::BUY);
  m.price = Price::fromDouble(100.07);  // a reference price some callers set
  h.exec.submitOrder(m);
  expectAccepted(h, 1);
}

// ---------------------------------------------------------------------------
// Ticks the symbol does not declare
// ---------------------------------------------------------------------------

TEST(OrderPriceTick, SymbolRegisteredWithoutATickIsNotChecked)
{
  Harness h;
  const SymbolId sym = addSymbolWithoutTick(h.reg, "DOGEUSDT");
  h.exec.submitOrder(limitOrder(1, sym, Side::BUY, 0.12345));
  expectAccepted(h, 1);
}

TEST(OrderPriceTick, DeclaredCentTickIsChecked)
{
  Harness h(0.01, /*known=*/true);
  h.exec.submitOrder(limitOrder(1, h.sym, Side::BUY, 100.005));
  expectRejected(h, 1, "price_off_tick");
}

TEST(OrderPriceTick, ZeroTickIsNotChecked)
{
  Harness h(0.0, /*known=*/true);
  h.exec.submitOrder(limitOrder(1, h.sym, Side::BUY, 100.00001));
  expectAccepted(h, 1);
}

TEST(OrderPriceTick, ExecutorWithoutARegistryChecksNothing)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&](const OrderEvent& ev)
                             { events.push_back(ev); });
  exec.submitOrder(limitOrder(1, 1, Side::BUY, 99.95));
  for (const auto& ev : events)
  {
    EXPECT_NE(ev.status, OrderEventStatus::REJECTED);
  }
}

// ---------------------------------------------------------------------------
// Which entries declare a tick, and whether the declaration survives a copy,
// a save and a load
// ---------------------------------------------------------------------------

namespace
{

std::filesystem::path scratchFile(const std::string& name)
{
  return std::filesystem::temp_directory_path() /
         (name + "_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
          ".json");
}

void writeText(const std::filesystem::path& path, const std::string& text)
{
  std::ofstream out(path, std::ios::binary);
  out << text;
}

int64_t registryOrderTick(const SymbolRegistry& reg, SymbolId sym)
{
  const auto info = reg.getSymbolInfo(sym);
  return info ? orderTickRaw(*info) : -1;
}

constexpr int64_t kCentRaw = Price::Scale / 100;

}  // namespace

TEST(OrderPriceTickDeclared, RegisterSymbolKeepsTheFlagOnTheStoredCopy)
{
  SymbolRegistry reg;
  const SymbolId declared = addSymbol(reg, "BTCUSDT", 0.01, /*known=*/true);
  ASSERT_TRUE(reg.getSymbolInfo(declared)->tickSizeKnown);
  EXPECT_EQ(registryOrderTick(reg, declared), kCentRaw);
  bool listed = false;
  for (const auto& info : reg.getAllSymbols())
  {
    if (info.id == declared)
    {
      listed = info.tickSizeKnown;
    }
  }
  EXPECT_TRUE(listed) << "getAllSymbols drops the declaration";
}

TEST(OrderPriceTickDeclared, CentTickAssignedWithoutTheFlagIsTheDefault)
{
  // A SymbolInfo cannot tell an assigned 0.01 from the default it starts
  // with, so in C++ an explicit 0.01 is declared with tickSizeKnown.
  SymbolRegistry reg;
  const SymbolId sym = addSymbol(reg, "BTCUSDT", 0.01, /*known=*/false);
  EXPECT_EQ(registryOrderTick(reg, sym), 0);
}

TEST(OrderPriceTickDeclared, NameOnlyRegistrationsDeclareNoTick)
{
  SymbolRegistry reg;
  const SymbolId byName = reg.registerSymbol("test", "BTCUSDT");
  const ExchangeId ex = reg.registerExchange("venue");
  const SymbolId byExchange = reg.registerSymbol(ex, "ETHUSDT");
  EXPECT_EQ(registryOrderTick(reg, byName), 0);
  EXPECT_EQ(registryOrderTick(reg, byExchange), 0);
}

TEST(OrderPriceTickDeclared, EquivalenceMappingKeepsTheFlag)
{
  SymbolRegistry reg;
  const SymbolId a = addSymbol(reg, "BTCUSDT", 0.01, /*known=*/true);
  SymbolInfo other;
  other.exchange = "other";
  other.symbol = "BTCUSDT";
  other.tickSize = kDefaultTickSize;
  other.tickSizeKnown = true;
  const SymbolId b = reg.registerSymbol(other);
  const std::vector<SymbolId> both{a, b};
  reg.mapEquivalentSymbols(both);
  EXPECT_EQ(registryOrderTick(reg, a), kCentRaw);
  EXPECT_EQ(registryOrderTick(reg, b), kCentRaw);
}

TEST(OrderPriceTickDeclared, SaveAndLoadKeepADeclaredCentTick)
{
  const auto path = scratchFile("lrvx_tick_declared");
  std::filesystem::remove(path);
  SymbolId declared = 0;
  SymbolId undeclared = 0;
  {
    SymbolRegistry reg;
    declared = addSymbol(reg, "BTCUSDT", 0.01, /*known=*/true);
    undeclared = addSymbolWithoutTick(reg, "ETHUSDT");
    ASSERT_TRUE(reg.saveToFile(path));
  }
  SymbolRegistry loaded;
  ASSERT_TRUE(loaded.loadFromFile(path));
  std::filesystem::remove(path);
  EXPECT_EQ(registryOrderTick(loaded, declared), kCentRaw)
      << "a 0.01 tick declared before the save is unchecked after the load";
  EXPECT_EQ(registryOrderTick(loaded, undeclared), 0);

  // And the executor reading the loaded registry refuses an off-tick price.
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setSymbolRegistry(&loaded);
  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&](const OrderEvent& ev)
                             { events.push_back(ev); });
  exec.submitOrder(limitOrder(1, declared, Side::BUY, 100.005));
  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events.front().status, OrderEventStatus::REJECTED);
  EXPECT_EQ(events.front().rejectReason, "price_off_tick");
}

TEST(OrderPriceTickDeclared, SerializeAndDeserializeKeepADeclaredCentTick)
{
  SymbolRegistry reg;
  const SymbolId declared = addSymbol(reg, "BTCUSDT", 0.01, /*known=*/true);
  const SymbolId undeclared = addSymbolWithoutTick(reg, "ETHUSDT");
  const auto bytes = reg.serialize();
  SymbolRegistry restored;
  ASSERT_TRUE(restored.deserialize(bytes));
  EXPECT_EQ(registryOrderTick(restored, declared), kCentRaw)
      << "a 0.01 tick declared before serialize is unchecked after deserialize";
  EXPECT_EQ(registryOrderTick(restored, undeclared), 0);
}

TEST(OrderPriceTickDeclared, RegistryFileWithoutTheFlagLoadsAsBefore)
{
  // A file written before the flag was persisted: no "tick_size_known". Its
  // 0.01 entry was never declared-checked and stays unchecked; any other tick
  // is checked, as it is for a symbol registered with it.
  const auto path = scratchFile("lrvx_tick_old_file");
  writeText(path,
            "{\n  \"version\": 1,\n  \"symbols\": [\n"
            "    {\"id\": 1, \"exchange\": \"binance\", \"symbol\": \"BTCUSDT\", \"type\": 0, "
            "\"tick_size\": 1000000, \"contract_multiplier\": 1, \"settlement_type\": 0, "
            "\"exercise_style\": 0},\n"
            "    {\"id\": 2, \"exchange\": \"binance\", \"symbol\": \"ETHUSDT\", \"type\": 0, "
            "\"tick_size\": 10000000}\n"
            "  ]\n}\n");
  SymbolRegistry loaded;
  ASSERT_TRUE(loaded.loadFromFile(path));
  std::filesystem::remove(path);
  ASSERT_EQ(loaded.size(), 2u);
  EXPECT_FALSE(loaded.getSymbolInfo(1)->tickSizeKnown);
  EXPECT_EQ(registryOrderTick(loaded, 1), 0);
  EXPECT_EQ(registryOrderTick(loaded, 2), Price::fromDouble(0.1).raw());
}

TEST(OrderPriceTickDeclared, SerializedRegistryWithoutTheFlagBitLoadsAsBefore)
{
  // An undeclared symbol serializes without the flag bit, the bytes a
  // registry written before the flag existed carries.
  SymbolRegistry reg;
  const SymbolId cent = addSymbol(reg, "BTCUSDT", 0.01, /*known=*/false);
  const SymbolId tenth = addSymbol(reg, "ETHUSDT", 0.1, /*known=*/false);
  const auto bytes = reg.serialize();
  SymbolRegistry restored;
  ASSERT_TRUE(restored.deserialize(bytes));
  EXPECT_FALSE(restored.getSymbolInfo(cent)->tickSizeKnown);
  EXPECT_EQ(registryOrderTick(restored, cent), 0);
  EXPECT_EQ(registryOrderTick(restored, tenth), Price::fromDouble(0.1).raw());
}

// ---------------------------------------------------------------------------
// Replace
// ---------------------------------------------------------------------------

TEST(OrderPriceTick, OffTickReplaceIsRejectedAndTheOriginalStays)
{
  Harness h;
  h.exec.onTrade(h.sym, Price::fromDouble(100.0), false);
  h.exec.submitOrder(limitOrder(1, h.sym, Side::BUY, 99.0));
  h.exec.replaceOrder(1, limitOrder(1, h.sym, Side::BUY, 99.05));

  const OrderEvent* ev = h.find(1, OrderEventStatus::REPLACE_REJECTED);
  ASSERT_NE(ev, nullptr);
  EXPECT_EQ(ev->rejectReason, "price_off_tick");
  EXPECT_EQ(ev->order.price, Price::fromDouble(99.0));
  EXPECT_EQ(h.find(1, OrderEventStatus::REPLACED), nullptr);

  // Still resting at 99.0: an on-tick replace finds it there.
  h.exec.replaceOrder(1, limitOrder(1, h.sym, Side::BUY, 99.1));
  const OrderEvent* replaced = h.find(1, OrderEventStatus::REPLACED);
  ASSERT_NE(replaced, nullptr);
  EXPECT_EQ(replaced->order.price, Price::fromDouble(99.0));
  EXPECT_EQ(replaced->newOrder.price, Price::fromDouble(99.1));
}

// ---------------------------------------------------------------------------
// Slippage: single-level fills snap against the owner, VWAPs stay averages
// ---------------------------------------------------------------------------

TEST(OrderPriceTick, BpsSlippedBuyIsSnappedUp)
{
  Harness h;
  SlippageProfile p;
  p.model = SlippageModel::FIXED_BPS;
  p.bps = 7.0;  // 100.0 -> 100.07
  h.exec.setDefaultSlippage(p);
  pushBook(h.exec, h.sym, {{99.9, 10.0}}, {{100.0, 10.0}});
  h.exec.submitOrder(marketOrder(1, h.sym, Side::BUY));
  const auto fills = h.fillsOf(1);
  ASSERT_EQ(fills.size(), 1u);
  EXPECT_EQ(fills[0].price, Price::fromDouble(100.1));
}

TEST(OrderPriceTick, BpsSlippedSellIsSnappedDown)
{
  Harness h;
  SlippageProfile p;
  p.model = SlippageModel::FIXED_BPS;
  p.bps = 7.0;  // 100.0 -> 99.93
  h.exec.setDefaultSlippage(p);
  pushBook(h.exec, h.sym, {{100.0, 10.0}}, {{100.1, 10.0}});
  h.exec.submitOrder(marketOrder(1, h.sym, Side::SELL));
  const auto fills = h.fillsOf(1);
  ASSERT_EQ(fills.size(), 1u);
  EXPECT_EQ(fills[0].price, Price::fromDouble(99.9));
}

TEST(OrderPriceTick, SlippedTriggerFillIsSnappedAgainstTheOwner)
{
  // A take-profit sell armed at 100.8 fills at its trigger, then pays 7 bps.
  Harness h;
  SlippageProfile p;
  p.model = SlippageModel::FIXED_BPS;
  p.bps = 7.0;
  h.exec.setDefaultSlippage(p);
  h.exec.onTrade(h.sym, Price::fromDouble(100.0), false);
  h.exec.submitOrder(conditional(1, h.sym, Side::SELL, OrderType::TAKE_PROFIT_MARKET, 100.8));
  h.exec.onBar(h.sym, Price::fromDouble(101.5), Price::fromDouble(100.0),
               Price::fromDouble(101.0));
  const auto fills = h.fillsOf(1);
  ASSERT_EQ(fills.size(), 1u);
  // 100.8 * (1 - 0.0007) = 100.72944 -> 100.7
  EXPECT_EQ(fills[0].price, Price::fromDouble(100.7));
}

TEST(OrderPriceTick, VwapOverSeveralLevelsStaysAnAverage)
{
  Harness h;
  pushBook(h.exec, h.sym, {{99.9, 10.0}}, {{100.0, 1.0}, {100.1, 2.0}});
  h.exec.submitOrder(marketOrder(1, h.sym, Side::BUY, 3.0));
  const auto fills = h.fillsOf(1);
  ASSERT_EQ(fills.size(), 1u);
  // (100.0 * 1 + 100.1 * 2) / 3 = 100.0666...
  EXPECT_NEAR(fills[0].price.toDouble(), 100.0666667, 1e-6);
}

TEST(OrderPriceTick, SlippedVwapIsNotSnapped)
{
  Harness h;
  SlippageProfile p;
  p.model = SlippageModel::FIXED_BPS;
  p.bps = 1.0;
  h.exec.setDefaultSlippage(p);
  pushBook(h.exec, h.sym, {{99.9, 10.0}}, {{100.0, 1.0}, {100.1, 2.0}});
  h.exec.submitOrder(marketOrder(1, h.sym, Side::BUY, 3.0));
  const auto fills = h.fillsOf(1);
  ASSERT_EQ(fills.size(), 1u);
  const double vwap = (100.0 + 2 * 100.1) / 3.0;
  EXPECT_NEAR(fills[0].price.toDouble(), vwap * 1.0001, 1e-6);
}

TEST(OrderPriceTick, SlippageOnAnUncheckedSymbolIsNotSnapped)
{
  Harness h(0.01, /*known=*/false);
  SlippageProfile p;
  p.model = SlippageModel::FIXED_BPS;
  p.bps = 7.0;
  h.exec.setDefaultSlippage(p);
  pushBook(h.exec, h.sym, {{99.9, 10.0}}, {{100.0, 10.0}});
  h.exec.submitOrder(marketOrder(1, h.sym, Side::BUY));
  const auto fills = h.fillsOf(1);
  ASSERT_EQ(fills.size(), 1u);
  EXPECT_NEAR(fills[0].price.toDouble(), 100.07, 1e-9);
}

// ---------------------------------------------------------------------------
// Percentage trailing stop: the trigger the simulator computes is on the tick
// ---------------------------------------------------------------------------

TEST(OrderPriceTick, PercentTrailingSellTriggerIsSnappedDown)
{
  Harness h;
  h.exec.onTrade(h.sym, Price::fromDouble(100.3), false);
  h.exec.submitOrder(trailing(1, h.sym, Side::SELL, 0.0, /*bps=*/100));
  // 100.3 * 0.99 = 99.297 -> 99.2; drop through it.
  h.exec.onTrade(h.sym, Price::fromDouble(99.25), false);
  EXPECT_TRUE(h.fillsOf(1).empty()) << "99.25 is above the snapped trigger 99.2";
  h.exec.onTrade(h.sym, Price::fromDouble(99.2), false);
  const OrderEvent* trig = h.find(1, OrderEventStatus::TRIGGERED);
  ASSERT_NE(trig, nullptr);
  EXPECT_EQ(trig->order.triggerPrice, Price::fromDouble(99.2));
}

TEST(OrderPriceTick, PercentTrailingBuyRatchetIsSnappedUp)
{
  Harness h;
  h.exec.onTrade(h.sym, Price::fromDouble(100.0), false);
  h.exec.submitOrder(trailing(1, h.sym, Side::BUY, 0.0, /*bps=*/100));
  h.exec.onTrade(h.sym, Price::fromDouble(99.3), false);
  // 99.3 * 1.01 = 100.293 -> 100.3
  Price last{};
  for (const auto& ev : h.events)
  {
    if (ev.order.id == 1 && ev.status == OrderEventStatus::TRAILING_UPDATED)
    {
      last = ev.newTrailingPrice;
    }
  }
  EXPECT_EQ(last, Price::fromDouble(100.3));
}

// ---------------------------------------------------------------------------
// The runner: bars and ticks, and what the strategy hears
// ---------------------------------------------------------------------------

namespace
{

BarEvent makeBar(SymbolId sym, double open, double high, double low, double close,
                 int64_t startNs)
{
  BarEvent ev{};
  ev.symbol = sym;
  ev.barType = BarType::Time;
  ev.barTypeParam = static_cast<uint64_t>(kMinuteNs);
  ev.bar.open = Price::fromDouble(open);
  ev.bar.high = Price::fromDouble(high);
  ev.bar.low = Price::fromDouble(low);
  ev.bar.close = Price::fromDouble(close);
  ev.bar.volume = Volume::fromDouble(1000.0);
  ev.bar.startTime = TimePoint{std::chrono::nanoseconds{startNs}};
  ev.bar.endTime = TimePoint{std::chrono::nanoseconds{startNs + kMinuteNs}};
  ev.bar.reason = BarCloseReason::Threshold;
  return ev;
}

// Buys on the first bar, places the exit `place` builds from the entry price on
// the next, and records every order event.
class EntryThenExit : public Strategy
{
 public:
  using Strategy::Strategy;

  SymbolId target{0};
  std::function<void(EntryThenExit&, double)> place;
  std::vector<OrderEvent> seen;
  double entry{0.0};
  size_t bars{0};

  void exitTakeProfit(double trigger)
  {
    emitTakeProfitMarket(target, Side::SELL, Price::fromDouble(trigger),
                         Quantity::fromDouble(1.0));
  }
  void exitLimit(double price)
  {
    emitLimitSell(target, Price::fromDouble(price), Quantity::fromDouble(1.0));
  }
  void exitStop(double trigger)
  {
    emitStopMarket(target, Side::SELL, Price::fromDouble(trigger), Quantity::fromDouble(1.0));
  }
  void exitTrailing(double offset)
  {
    emitTrailingStop(target, Side::SELL, Price::fromDouble(offset), Quantity::fromDouble(1.0));
  }

 protected:
  void onSymbolBar(SymbolContext& /*ctx*/, const BarEvent& /*ev*/) override
  {
    if (bars == 0)
    {
      emitMarketBuy(target, Quantity::fromDouble(1.0));
    }
    else if (bars == 1 && entry > 0.0)
    {
      place(*this, entry);
    }
    ++bars;
  }

  void onSymbolOrderUpdate(SymbolContext& /*ctx*/, const OrderEvent& ev) override
  {
    seen.push_back(ev);
  }
};

}  // namespace

class OrderPriceTickRunner : public ::testing::Test
{
 protected:
  SymbolRegistry reg;
  SymbolId sym{0};

  void SetUp() override { sym = addSymbol(reg, "BTCUSDT", 0.1); }

  std::vector<BarEvent> bars() const
  {
    return {
        makeBar(sym, 100.0, 100.0, 100.0, 100.0, 0),
        makeBar(sym, 100.3, 100.4, 100.2, 100.3, kMinuteNs),
        makeBar(sym, 100.3, 101.5, 100.0, 101.0, 2 * kMinuteNs),
        makeBar(sym, 101.0, 102.0, 99.0, 101.5, 3 * kMinuteNs),
    };
  }

  // Runs the bars; returns the exit order's events and the runner's fills.
  std::pair<std::vector<OrderEvent>, std::vector<Fill>> runBars(
      std::function<void(EntryThenExit&, double)> place, bool withRegistry = true)
  {
    EntryThenExit strat(1, std::vector<SymbolId>{sym}, reg);
    strat.target = sym;
    strat.place = std::move(place);
    BacktestRunner runner;
    if (withRegistry)
    {
      runner.setSymbolRegistry(&reg);
    }
    runner.setStrategy(&strat);
    // The entry fills at bar 1's open; the strategy reads it from there.
    strat.entry = 100.3;
    runner.runBars(bars());
    std::vector<OrderEvent> exits;
    for (const auto& ev : strat.seen)
    {
      if (ev.order.type != OrderType::MARKET)
      {
        exits.push_back(ev);
      }
    }
    return {exits, runner.executor().fills()};
  }
};

TEST_F(OrderPriceTickRunner, TakeProfitFromTheFillIsRejectedOnBars)
{
  auto [exits, fills] = runBars([](EntryThenExit& s, double e)
                                { s.exitTakeProfit(e * 1.005); });
  ASSERT_FALSE(exits.empty());
  EXPECT_EQ(exits.front().status, OrderEventStatus::REJECTED);
  EXPECT_EQ(exits.front().rejectReason, "trigger_off_tick");
  EXPECT_EQ(fills.size(), 1u) << "only the entry fills";
}

TEST_F(OrderPriceTickRunner, OffTickLimitStopAndTrailingAreRejectedOnBars)
{
  auto [limitExits, f1] = runBars([](EntryThenExit& s, double e)
                                  { s.exitLimit(e * 1.0037); });
  ASSERT_FALSE(limitExits.empty());
  EXPECT_EQ(limitExits.front().rejectReason, "price_off_tick");

  auto [stopExits, f2] = runBars([](EntryThenExit& s, double e)
                                 { s.exitStop(e * 0.9963); });
  ASSERT_FALSE(stopExits.empty());
  EXPECT_EQ(stopExits.front().rejectReason, "trigger_off_tick");

  auto [trailExits, f3] = runBars([](EntryThenExit& s, double)
                                  { s.exitTrailing(0.25); });
  ASSERT_FALSE(trailExits.empty());
  EXPECT_EQ(trailExits.front().rejectReason, "trailing_offset_off_tick");
}

TEST_F(OrderPriceTickRunner, OnTickTakeProfitFillsAtItsTriggerOnBars)
{
  auto [exits, fills] = runBars([](EntryThenExit& s, double)
                                { s.exitTakeProfit(100.8); });
  for (const auto& ev : exits)
  {
    EXPECT_NE(ev.status, OrderEventStatus::REJECTED);
  }
  ASSERT_EQ(fills.size(), 2u);
  EXPECT_EQ(fills[1].price, Price::fromDouble(100.8));
}

TEST_F(OrderPriceTickRunner, RunnerWithoutARegistryReadsTheStrategysTicks)
{
  // No setSymbolRegistry: the runner takes the ticks the strategy was built
  // with, as the Python, Node, Codon and C API runners do.
  auto [exits, fills] =
      runBars([](EntryThenExit& s, double e)
              { s.exitTakeProfit(e * 1.005); }, false);
  ASSERT_FALSE(exits.empty());
  EXPECT_EQ(exits.front().status, OrderEventStatus::REJECTED);
  EXPECT_EQ(exits.front().rejectReason, "trigger_off_tick");
  EXPECT_EQ(fills.size(), 1u) << "only the entry fills";
}

TEST_F(OrderPriceTickRunner, StrategysTicksOutliveItsRegistry)
{
  // The strategy keeps a copy of each symbol's entry, so a registry that is
  // gone by the time the run starts still decides the tick.
  auto scratch = std::make_unique<SymbolRegistry>();
  const SymbolId local = addSymbol(*scratch, "BTCUSDT", 0.1);
  EntryThenExit strat(1, std::vector<SymbolId>{local}, *scratch);
  scratch.reset();
  strat.target = local;
  strat.place = [](EntryThenExit& s, double e)
  { s.exitLimit(e * 1.0037); };
  strat.entry = 100.3;
  BacktestRunner runner;
  runner.setStrategy(&strat);
  runner.runBars(bars());
  bool rejected = false;
  for (const auto& ev : strat.seen)
  {
    rejected = rejected || (ev.status == OrderEventStatus::REJECTED &&
                            ev.rejectReason == "price_off_tick");
  }
  EXPECT_TRUE(rejected);
}

TEST_F(OrderPriceTickRunner, ExplicitRegistryWinsOverTheStrategys)
{
  // The same id with no declared tick in the runner's registry: unchecked,
  // whichever of the two calls comes first.
  SymbolRegistry undeclared;
  ASSERT_EQ(addSymbolWithoutTick(undeclared, "BTCUSDT"), sym);
  for (const bool registryFirst : {true, false})
  {
    EntryThenExit strat(1, std::vector<SymbolId>{sym}, reg);
    strat.target = sym;
    strat.place = [](EntryThenExit& s, double e)
    { s.exitTakeProfit(e * 1.005); };
    strat.entry = 100.3;
    BacktestRunner runner;
    if (registryFirst)
    {
      runner.setSymbolRegistry(&undeclared);
      runner.setStrategy(&strat);
    }
    else
    {
      runner.setStrategy(&strat);
      runner.setSymbolRegistry(&undeclared);
    }
    runner.runBars(bars());
    const auto& fills = runner.executor().fills();
    ASSERT_EQ(fills.size(), 2u) << "registryFirst=" << registryFirst;
    EXPECT_NEAR(fills[1].price.toDouble(), 100.8015, 1e-9);
  }
}

TEST_F(OrderPriceTickRunner, VenueExecutorGetsTheStrategysTicks)
{
  SimulatedClock venueClock;
  SimulatedExecutor venue(venueClock);
  EntryThenExit strat(1, std::vector<SymbolId>{sym}, reg);
  strat.target = sym;
  strat.place = [](EntryThenExit& s, double e)
  { s.exitStop(e * 0.9963); };
  strat.entry = 100.3;
  BacktestRunner runner;
  runner.setStrategy(&strat);
  runner.setSimulatedExecutor(&venue, &venueClock);
  EXPECT_EQ(venue.orderTickRaw(sym), Price::fromDouble(0.1).raw());
  runner.runBars(bars());
  bool rejected = false;
  for (const auto& ev : strat.seen)
  {
    rejected = rejected || ev.rejectReason == "trigger_off_tick";
  }
  EXPECT_TRUE(rejected);
}

namespace
{

// An IStrategy that is not an lrvx::Strategy: it was never handed a registry,
// so the runner has no tick to read from it.
class BareStrategy : public IStrategy
{
 public:
  SubscriberId id() const override { return 7; }
  void setSignalHandler(ISignalHandler* h) override { _handler = h; }
  void onBar(const BarEvent& ev) override
  {
    if (_bars++ == 0)
    {
      _handler->onSignal(
          Signal::limitBuy(ev.symbol, Price::fromDouble(100.05), Quantity::fromDouble(1.0), 1));
    }
  }
  void onOrderEvent(const OrderEvent& ev) override { seen.push_back(ev); }
  std::vector<OrderEvent> seen;

 private:
  ISignalHandler* _handler{nullptr};
  int _bars{0};
};

}  // namespace

TEST_F(OrderPriceTickRunner, StrategyWithoutSymbolInfoNeedsTheRegistry)
{
  {
    BareStrategy strat;
    BacktestRunner runner;
    runner.setStrategy(&strat);
    runner.runBars(bars());
    ASSERT_FALSE(strat.seen.empty());
    for (const auto& ev : strat.seen)
    {
      EXPECT_NE(ev.status, OrderEventStatus::REJECTED);
    }
  }
  {
    BareStrategy strat;
    BacktestRunner runner;
    runner.setSymbolRegistry(&reg);
    runner.setStrategy(&strat);
    runner.runBars(bars());
    ASSERT_FALSE(strat.seen.empty());
    EXPECT_EQ(strat.seen.front().rejectReason, "price_off_tick");
  }
}

TEST(OrderPriceTickTicks, TakeProfitFromTheFillIsRejectedOnTicks)
{
  SymbolRegistry reg;
  const SymbolId sym = addSymbol(reg, "BTCUSDT", 0.1);
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setSymbolRegistry(&reg);
  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&](const OrderEvent& ev)
                             { events.push_back(ev); });

  exec.onTrade(sym, Price::fromDouble(100.3), Quantity::fromDouble(1.0), true);
  exec.submitOrder(marketOrder(1, sym, Side::BUY));
  ASSERT_EQ(exec.fills().size(), 1u);
  const double entry = exec.fills()[0].price.toDouble();
  exec.submitOrder(
      conditional(2, sym, Side::SELL, OrderType::TAKE_PROFIT_MARKET, entry * 1.005));
  exec.onTrade(sym, Price::fromDouble(101.0), Quantity::fromDouble(1.0), true);

  bool rejected = false;
  for (const auto& ev : events)
  {
    if (ev.order.id == 2 && ev.status == OrderEventStatus::REJECTED)
    {
      rejected = true;
      EXPECT_EQ(ev.rejectReason, "trigger_off_tick");
    }
  }
  EXPECT_TRUE(rejected);
  EXPECT_EQ(exec.fills().size(), 1u);
}
