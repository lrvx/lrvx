/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Order quantities on the lot. A venue refuses an order whose quantity is not
// a whole number of its quantity step, so the simulator refuses it too when
// the symbol declares one. A symbol declares no lot unless it is given one,
// and then nothing about its quantities is checked. The declared lot is 0.001
// throughout unless a test says otherwise.

#include "lrvx/aggregator/events/bar_event.h"
#include "lrvx/backtest/backtest_runner.h"
#include "lrvx/backtest/simulated_clock.h"
#include "lrvx/backtest/simulated_executor.h"
#include "lrvx/engine/symbol_registry.h"
#include "lrvx/execution/order_price_rule.h"
#include "lrvx/strategy/strategy.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace lrvx;

namespace
{

constexpr int64_t kMinuteNs = 60'000'000'000LL;
const int64_t kMilliLot = Quantity::fromDouble(0.001).raw();

SymbolId addSymbol(SymbolRegistry& reg, const std::string& name, double lot)
{
  SymbolInfo info;
  info.exchange = "test";
  info.symbol = name;
  info.tickSize = Price::fromDouble(0.1);
  info.tickSizeKnown = true;
  if (lot > 0.0)
  {
    info.lotSize = Quantity::fromDouble(lot);
  }
  return reg.registerSymbol(info);
}

Order limitOrder(OrderId id, SymbolId sym, Side side, double price, double qty)
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

struct Harness
{
  SymbolRegistry reg;
  SimulatedClock clock;
  SimulatedExecutor exec{clock};
  std::vector<OrderEvent> events;
  SymbolId sym{0};

  explicit Harness(double lot = 0.001)
  {
    sym = addSymbol(reg, "BTCUSDT", lot);
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
};

void expectRejected(const Harness& h, OrderId id, const char* reason)
{
  const OrderEvent* ev = h.find(id, OrderEventStatus::REJECTED);
  ASSERT_NE(ev, nullptr) << "order " << id << " was not rejected";
  EXPECT_EQ(ev->rejectReason, reason);
  EXPECT_EQ(h.find(id, OrderEventStatus::SUBMITTED), nullptr)
      << "a refused order never reaches the book";
}

void expectAccepted(const Harness& h, OrderId id)
{
  EXPECT_EQ(h.find(id, OrderEventStatus::REJECTED), nullptr) << "order " << id << " rejected";
  EXPECT_NE(h.find(id, OrderEventStatus::SUBMITTED), nullptr);
}

std::filesystem::path scratchFile(const std::string& stem)
{
  std::ostringstream name;
  name << stem << "_" << std::this_thread::get_id() << "_"
       << std::chrono::steady_clock::now().time_since_epoch().count() << ".json";
  return std::filesystem::temp_directory_path() / name.str();
}

std::string readText(const std::filesystem::path& path)
{
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

uint32_t serializedVersion(const std::vector<std::byte>& bytes)
{
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i)
  {
    v |= static_cast<uint32_t>(static_cast<uint8_t>(bytes[4 + i])) << (8 * i);
  }
  return v;
}

}  // namespace

// ---------------------------------------------------------------------------
// The rule
// ---------------------------------------------------------------------------

TEST(OrderQuantityRule, ASymbolDeclaresNoLotByDefault)
{
  SymbolInfo info;
  EXPECT_EQ(info.lotSize.raw(), 0);
  EXPECT_EQ(orderLotRaw(info), 0);
  info.lotSize = Quantity::fromDouble(0.001);
  EXPECT_EQ(orderLotRaw(info), kMilliLot);
  info.lotSize = Quantity::fromRaw(-5);
  EXPECT_EQ(orderLotRaw(info), 0) << "a lot that is not positive is not a lot";
}

TEST(OrderQuantityRule, AQuantityOffTheLotHasAReason)
{
  Order o = limitOrder(1, 1, Side::BUY, 100.0, 0.0015);
  EXPECT_STREQ(offLotReason(o, kMilliLot), "quantity_off_lot");
  EXPECT_STREQ(kRejectQuantityOffLot, "quantity_off_lot");
  o.quantity = Quantity::fromDouble(0.002);
  EXPECT_EQ(offLotReason(o, kMilliLot), nullptr);
  o.type = OrderType::MARKET;
  o.quantity = Quantity::fromDouble(0.0015);
  EXPECT_STREQ(offLotReason(o, kMilliLot), "quantity_off_lot") << "a market order has a quantity";
  EXPECT_EQ(offLotReason(o, 0), nullptr) << "no lot, no check";
}

TEST(OrderQuantityRule, AnIcebergsVisibleQuantityIsCheckedToo)
{
  Order o = limitOrder(1, 1, Side::BUY, 100.0, 1.0);
  o.type = OrderType::ICEBERG;
  o.visibleQuantity = Quantity::fromDouble(0.0105);
  EXPECT_STREQ(offLotReason(o, kMilliLot), "quantity_off_lot");
  o.visibleQuantity = Quantity::fromDouble(0.01);
  EXPECT_EQ(offLotReason(o, kMilliLot), nullptr);
}

// ---------------------------------------------------------------------------
// The simulator
// ---------------------------------------------------------------------------

TEST(OrderQuantityLot, OffLotLimitIsRejected)
{
  Harness h;
  h.exec.submitOrder(limitOrder(1, h.sym, Side::BUY, 99.0, 0.0015));
  expectRejected(h, 1, "quantity_off_lot");
  EXPECT_EQ(h.exec.orderLotRaw(h.sym), kMilliLot);
}

TEST(OrderQuantityLot, OffLotMarketOrderIsRejected)
{
  Harness h;
  h.exec.onTrade(h.sym, Price::fromDouble(100.0), false);
  Order o = limitOrder(1, h.sym, Side::BUY, 0.0, 1.0005);
  o.type = OrderType::MARKET;
  h.exec.submitOrder(o);
  expectRejected(h, 1, "quantity_off_lot");
  EXPECT_TRUE(h.exec.fills().empty());
}

TEST(OrderQuantityLot, OnLotOrderIsAccepted)
{
  Harness h;
  h.exec.onTrade(h.sym, Price::fromDouble(100.0), false);
  h.exec.submitOrder(limitOrder(1, h.sym, Side::BUY, 99.0, 0.002));
  h.exec.submitOrder(limitOrder(2, h.sym, Side::BUY, 99.0, 3.0));
  expectAccepted(h, 1);
  expectAccepted(h, 2);
}

TEST(OrderQuantityLot, ASymbolWithoutALotTakesAnyQuantity)
{
  Harness h(0.0);
  h.exec.onTrade(h.sym, Price::fromDouble(100.0), false);
  h.exec.submitOrder(limitOrder(1, h.sym, Side::BUY, 99.0, 0.00012345));
  expectAccepted(h, 1);
  EXPECT_EQ(h.exec.orderLotRaw(h.sym), 0);
}

TEST(OrderQuantityLot, ThePriceIsCheckedFirst)
{
  Harness h;
  h.exec.submitOrder(limitOrder(1, h.sym, Side::BUY, 99.05, 0.0015));
  expectRejected(h, 1, "price_off_tick");
}

TEST(OrderQuantityLot, WithoutARegistryNothingIsChecked)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&](const OrderEvent& ev)
                             { events.push_back(ev); });
  exec.submitOrder(limitOrder(1, 1, Side::BUY, 99.0, 0.0015));
  for (const auto& ev : events)
  {
    EXPECT_NE(ev.status, OrderEventStatus::REJECTED);
  }
  EXPECT_EQ(exec.orderLotRaw(1), 0);
}

TEST(OrderQuantityLot, OffLotReplaceIsRejectedAndTheOriginalStays)
{
  Harness h;
  h.exec.onTrade(h.sym, Price::fromDouble(100.0), false);
  h.exec.submitOrder(limitOrder(1, h.sym, Side::BUY, 99.0, 0.002));
  h.exec.replaceOrder(1, limitOrder(1, h.sym, Side::BUY, 99.0, 0.0025));

  const OrderEvent* ev = h.find(1, OrderEventStatus::REPLACE_REJECTED);
  ASSERT_NE(ev, nullptr);
  EXPECT_EQ(ev->rejectReason, "quantity_off_lot");
  EXPECT_EQ(ev->order.quantity, Quantity::fromDouble(0.002));
  EXPECT_EQ(h.find(1, OrderEventStatus::REPLACED), nullptr);

  // Still resting with 0.002: an on-lot replace finds it there.
  h.exec.replaceOrder(1, limitOrder(1, h.sym, Side::BUY, 99.0, 0.003));
  EXPECT_NE(h.find(1, OrderEventStatus::REPLACED), nullptr);
}

// ---------------------------------------------------------------------------
// Through a strategy and the backtest runner
// ---------------------------------------------------------------------------

namespace
{

class OneOrder : public Strategy
{
 public:
  using Strategy::Strategy;
  double qty{0.0};
  std::vector<OrderEvent> seen;

 protected:
  void onSymbolBar(SymbolContext& /*ctx*/, const BarEvent& ev) override
  {
    if (_bars++ == 0)
    {
      emitLimitBuy(ev.symbol, Price::fromDouble(99.0), Quantity::fromDouble(qty));
    }
  }
  void onSymbolOrderUpdate(SymbolContext& /*ctx*/, const OrderEvent& ev) override
  {
    seen.push_back(ev);
  }

 private:
  int _bars{0};
};

std::vector<BarEvent> twoBars(SymbolId sym)
{
  std::vector<BarEvent> out;
  for (int i = 0; i < 2; ++i)
  {
    BarEvent ev{};
    ev.symbol = sym;
    ev.barType = BarType::Time;
    ev.barTypeParam = static_cast<uint64_t>(kMinuteNs);
    ev.bar.open = Price::fromDouble(100.0);
    ev.bar.high = Price::fromDouble(100.5);
    ev.bar.low = Price::fromDouble(99.5);
    ev.bar.close = Price::fromDouble(100.0);
    ev.bar.startTime = TimePoint{std::chrono::nanoseconds{kMinuteNs * i}};
    ev.bar.endTime = TimePoint{std::chrono::nanoseconds{kMinuteNs * (i + 1)}};
    out.push_back(ev);
  }
  return out;
}

std::string firstRejectReason(double lot, double qty, bool withRegistry)
{
  SymbolRegistry reg;
  const SymbolId sym = addSymbol(reg, "BTCUSDT", lot);
  OneOrder strat(1, std::vector<SymbolId>{sym}, reg);
  strat.qty = qty;
  BacktestRunner runner;
  if (withRegistry)
  {
    runner.setSymbolRegistry(&reg);
  }
  runner.setStrategy(&strat);
  runner.runBars(twoBars(sym));
  for (const auto& ev : strat.seen)
  {
    if (ev.status == OrderEventStatus::REJECTED)
    {
      return ev.rejectReason;
    }
  }
  return "";
}

}  // namespace

TEST(OrderQuantityLotRunner, AStrategysOffLotOrderIsRejected)
{
  EXPECT_EQ(firstRejectReason(0.001, 0.0015, true), "quantity_off_lot");
  EXPECT_EQ(firstRejectReason(0.001, 0.002, true), "");
  EXPECT_EQ(firstRejectReason(0.0, 0.0015, true), "");
}

TEST(OrderQuantityLotRunner, TheRunnerReadsTheLotFromTheStrategysSymbols)
{
  // No setSymbolRegistry: the runner checks against the entries the strategy
  // copied, as it does for the tick.
  EXPECT_EQ(firstRejectReason(0.001, 0.0015, false), "quantity_off_lot");
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

TEST(OrderQuantityLotPersistence, SaveAndLoadKeepADeclaredLot)
{
  const auto path = scratchFile("lrvx_lot_declared");
  SymbolId declared = 0;
  SymbolId undeclared = 0;
  {
    SymbolRegistry reg;
    declared = addSymbol(reg, "BTCUSDT", 0.001);
    undeclared = addSymbol(reg, "ETHUSDT", 0.0);
    ASSERT_TRUE(reg.saveToFile(path));
  }
  SymbolRegistry loaded;
  ASSERT_TRUE(loaded.loadFromFile(path));
  std::filesystem::remove(path);
  EXPECT_EQ(loaded.getSymbolInfo(declared)->lotSize.raw(), kMilliLot);
  EXPECT_EQ(loaded.getSymbolInfo(undeclared)->lotSize.raw(), 0);

  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setSymbolRegistry(&loaded);
  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&](const OrderEvent& ev)
                             { events.push_back(ev); });
  exec.submitOrder(limitOrder(1, declared, Side::BUY, 99.0, 0.0015));
  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events.front().status, OrderEventStatus::REJECTED);
  EXPECT_EQ(events.front().rejectReason, "quantity_off_lot");
}

TEST(OrderQuantityLotPersistence, AnEntryWithoutALotIsWrittenAsBefore)
{
  const auto path = scratchFile("lrvx_lot_undeclared");
  SymbolRegistry reg;
  addSymbol(reg, "BTCUSDT", 0.0);
  ASSERT_TRUE(reg.saveToFile(path));
  const std::string text = readText(path);
  std::filesystem::remove(path);
  EXPECT_EQ(text.find("lot_size"), std::string::npos) << text;
}

TEST(OrderQuantityLotPersistence, AFileFromBeforeLotsLoadsWithNone)
{
  const auto path = scratchFile("lrvx_lot_old_file");
  {
    std::ofstream out(path, std::ios::binary);
    out << "{\n  \"version\": 1,\n  \"symbols\": [\n"
           "    {\"id\": 1, \"exchange\": \"binance\", \"symbol\": \"BTCUSDT\", \"type\": 0, "
           "\"tick_size\": 10000000, \"tick_size_known\": true, \"contract_multiplier\": 1, "
           "\"settlement_type\": 0, \"exercise_style\": 0}\n"
           "  ]\n}\n";
  }
  SymbolRegistry loaded;
  ASSERT_TRUE(loaded.loadFromFile(path));
  std::filesystem::remove(path);
  ASSERT_EQ(loaded.size(), 1u);
  EXPECT_EQ(loaded.getSymbolInfo(1)->lotSize.raw(), 0);
  EXPECT_EQ(orderLotRaw(*loaded.getSymbolInfo(1)), 0);
  EXPECT_TRUE(loaded.getSymbolInfo(1)->tickSizeKnown);
}

TEST(OrderQuantityLotPersistence, SerializeAndDeserializeKeepADeclaredLot)
{
  SymbolRegistry reg;
  const SymbolId declared = addSymbol(reg, "BTCUSDT", 0.001);
  const SymbolId undeclared = addSymbol(reg, "ETHUSDT", 0.0);
  SymbolInfo option;
  option.exchange = "test";
  option.symbol = "BTC-C";
  option.type = InstrumentType::Option;
  option.strike = Price::fromDouble(50000.0);
  option.settlementCcy = "USDC";
  option.lotSize = Quantity::fromDouble(0.1);
  const SymbolId opt = reg.registerSymbol(option);

  const auto bytes = reg.serialize();
  EXPECT_EQ(serializedVersion(bytes), 5u);
  SymbolRegistry restored;
  ASSERT_TRUE(restored.deserialize(bytes));
  EXPECT_EQ(restored.getSymbolInfo(declared)->lotSize.raw(), kMilliLot);
  EXPECT_EQ(restored.getSymbolInfo(undeclared)->lotSize.raw(), 0);
  EXPECT_EQ(restored.getSymbolInfo(opt)->lotSize.raw(), Quantity::fromDouble(0.1).raw());
  EXPECT_EQ(*restored.getSymbolInfo(opt)->settlementCcy, "USDC");
  EXPECT_EQ(restored.getSymbolInfo(opt)->strike->raw(), Price::fromDouble(50000.0).raw());
}

TEST(OrderQuantityLotPersistence, ARegistryWithoutLotsSerializesAsBefore)
{
  // No lot declared: the bytes are the version-4 form a build from before
  // lots reads, and they load with no lot.
  SymbolRegistry reg;
  const SymbolId sym = addSymbol(reg, "BTCUSDT", 0.0);
  auto bytes = reg.serialize();
  EXPECT_EQ(serializedVersion(bytes), 4u);
  SymbolRegistry restored;
  ASSERT_TRUE(restored.deserialize(bytes));
  EXPECT_EQ(restored.getSymbolInfo(sym)->lotSize.raw(), 0);

  // In version 4 the lot bit means nothing and no bytes follow it.
  // header 12, id 4, "test" 2+4, "BTCUSDT" 2+7, type 1, tick 8, then flags.
  const size_t flagsAt = 12 + 4 + 6 + 9 + 1 + 8;
  ASSERT_LT(flagsAt, bytes.size());
  bytes[flagsAt] = static_cast<std::byte>(static_cast<uint8_t>(bytes[flagsAt]) | 0x20);
  SymbolRegistry flagged;
  ASSERT_TRUE(flagged.deserialize(bytes));
  ASSERT_EQ(flagged.size(), 1u);
  EXPECT_EQ(flagged.getSymbolInfo(sym)->lotSize.raw(), 0);
  EXPECT_EQ(flagged.getSymbolInfo(sym)->symbol, "BTCUSDT");
  EXPECT_EQ(flagged.getSymbolInfo(sym)->qtyScale, Quantity::Scale);
}

// ---------------------------------------------------------------------------
// The venue's lot
// ---------------------------------------------------------------------------

TEST(SymbolRegistryVenueLot, AnUndeclaredLotTakesTheVenues)
{
  SymbolRegistry registry;
  const SymbolId id = addSymbol(registry, "BTCUSDT", 0.0);
  const SymbolInfo before = *registry.getSymbolInfo(id);
  const uint64_t rev = registry.venueTickLog()->revision();

  const auto r = registry.applyVenueLot(id, Quantity::fromDouble(0.001));
  EXPECT_EQ(r.outcome, VenueTickOutcome::Applied);
  EXPECT_EQ(r.heldLot.raw(), kMilliLot);

  const SymbolInfo after = *registry.getSymbolInfo(id);
  EXPECT_EQ(after.lotSize.raw(), kMilliLot);
  EXPECT_EQ(after.tickSize.raw(), before.tickSize.raw());
  EXPECT_EQ(after.tickSizeKnown, before.tickSizeKnown);
  EXPECT_EQ(after.symbol, before.symbol);
  ASSERT_TRUE(registry.venueTickLog()->lotFor(id));
  EXPECT_EQ(registry.venueTickLog()->lotFor(id)->raw(), kMilliLot);
  EXPECT_GT(registry.venueTickLog()->revision(), rev) << "an executor's lot cache follows it";
  EXPECT_FALSE(registry.venueTickLog()->tickFor(id));
}

TEST(SymbolRegistryVenueLot, ADeclaredLotIsKept)
{
  SymbolRegistry registry;
  const SymbolId same = addSymbol(registry, "BTCUSDT", 0.001);
  const SymbolId other = addSymbol(registry, "ETHUSDT", 0.01);

  auto r = registry.applyVenueLot(same, Quantity::fromDouble(0.001));
  EXPECT_EQ(r.outcome, VenueTickOutcome::Unchanged);
  r = registry.applyVenueLot(other, Quantity::fromDouble(0.001));
  EXPECT_EQ(r.outcome, VenueTickOutcome::Conflict);
  EXPECT_EQ(r.heldLot.raw(), Quantity::fromDouble(0.01).raw());
  EXPECT_EQ(registry.getSymbolInfo(other)->lotSize.raw(), Quantity::fromDouble(0.01).raw());
  EXPECT_FALSE(registry.venueTickLog()->lotFor(other));

  // A declared lot equal to the venue's is still the caller's: the venue
  // moving away from it is a conflict, not an update.
  r = registry.applyVenueLot(same, Quantity::fromDouble(0.01));
  EXPECT_EQ(r.outcome, VenueTickOutcome::Conflict);
  EXPECT_EQ(registry.getSymbolInfo(same)->lotSize.raw(), kMilliLot);
}

TEST(SymbolRegistryVenueLot, AnAppliedLotFollowsTheVenueAgain)
{
  SymbolRegistry registry;
  const SymbolId id = addSymbol(registry, "BTCUSDT", 0.0);
  ASSERT_EQ(registry.applyVenueLot(id, Quantity::fromDouble(0.001)).outcome,
            VenueTickOutcome::Applied);
  auto r = registry.applyVenueLot(id, Quantity::fromDouble(0.01));
  EXPECT_EQ(r.outcome, VenueTickOutcome::Applied);
  EXPECT_EQ(registry.getSymbolInfo(id)->lotSize.raw(), Quantity::fromDouble(0.01).raw());
  r = registry.applyVenueLot(id, Quantity::fromDouble(0.01));
  EXPECT_EQ(r.outcome, VenueTickOutcome::Unchanged);
}

TEST(SymbolRegistryVenueLot, UnknownIdsAndNonPositiveLotsAreRefused)
{
  SymbolRegistry registry;
  const SymbolId id = addSymbol(registry, "BTCUSDT", 0.0);
  EXPECT_EQ(registry.applyVenueLot(id + 100, Quantity::fromDouble(0.001)).outcome,
            VenueTickOutcome::UnknownSymbol);
  EXPECT_EQ(registry.applyVenueLot(id, Quantity{}).outcome, VenueTickOutcome::InvalidTick);
  EXPECT_EQ(registry.getSymbolInfo(id)->lotSize.raw(), 0);
}

TEST(SymbolRegistryVenueLot, ALotLoadedFromAFileIsDeclared)
{
  // What the venue applied is not remembered across a save: a loaded lot is
  // the file's, and the venue does not replace it.
  SymbolRegistry registry;
  const SymbolId id = addSymbol(registry, "BTCUSDT", 0.0);
  registry.applyVenueLot(id, Quantity::fromDouble(0.001));
  const auto bytes = registry.serialize();
  SymbolRegistry restored;
  ASSERT_TRUE(restored.deserialize(bytes));
  EXPECT_EQ(restored.applyVenueLot(id, Quantity::fromDouble(0.01)).outcome,
            VenueTickOutcome::Conflict);
  EXPECT_EQ(restored.getSymbolInfo(id)->lotSize.raw(), kMilliLot);
}

TEST(SymbolRegistryVenueLot, TheSimulatorChecksAnAppliedLot)
{
  Harness h(0.0);
  h.reg.applyVenueLot(h.sym, Quantity::fromDouble(0.001));
  h.exec.submitOrder(limitOrder(1, h.sym, Side::BUY, 99.0, 0.0015));
  expectRejected(h, 1, "quantity_off_lot");
}

// ---------------------------------------------------------------------------
// Values applied after the executor's first read
// ---------------------------------------------------------------------------

// The executor caches each symbol's tick and lot on first use, "none known"
// included. A value a connector applies after that read must still be picked
// up by the same executor.
TEST(SymbolRegistryVenueLot, AnExecutorThatAlreadyReadFollowsAnAppliedLot)
{
  Harness h(0.0);
  h.exec.submitOrder(limitOrder(1, h.sym, Side::BUY, 99.0, 0.0015));
  expectAccepted(h, 1);
  ASSERT_EQ(h.exec.orderLotRaw(h.sym), 0);

  ASSERT_EQ(h.reg.applyVenueLot(h.sym, Quantity::fromDouble(0.001)).outcome,
            VenueTickOutcome::Applied);
  EXPECT_EQ(h.exec.orderLotRaw(h.sym), kMilliLot);
  h.exec.submitOrder(limitOrder(2, h.sym, Side::BUY, 99.0, 0.0015));
  expectRejected(h, 2, "quantity_off_lot");

  // And the venue's next lot after that.
  ASSERT_EQ(h.reg.applyVenueLot(h.sym, Quantity::fromDouble(0.0005)).outcome,
            VenueTickOutcome::Applied);
  h.exec.submitOrder(limitOrder(3, h.sym, Side::BUY, 99.0, 0.0015));
  expectAccepted(h, 3);
}

TEST(SymbolRegistryVenueLot, AnExecutorThatAlreadyReadFollowsAnAppliedTick)
{
  SymbolRegistry reg;
  SymbolInfo info;
  info.exchange = "test";
  info.symbol = "UNTICKED";
  const SymbolId sym = reg.registerSymbol(info);
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  std::vector<OrderEvent> events;
  exec.setSymbolRegistry(&reg);
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  exec.submitOrder(limitOrder(1, sym, Side::BUY, 99.05, 1.0));
  ASSERT_EQ(exec.orderTickRaw(sym), 0);
  ASSERT_EQ(reg.applyVenueTick(sym, Price::fromDouble(0.1)).outcome, VenueTickOutcome::Applied);
  EXPECT_EQ(exec.orderTickRaw(sym), Price::fromDouble(0.1).raw());

  events.clear();
  exec.submitOrder(limitOrder(2, sym, Side::BUY, 99.05, 1.0));
  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events.front().status, OrderEventStatus::REJECTED);
  EXPECT_EQ(events.front().rejectReason, "price_off_tick");
}

TEST(SymbolRegistryVenueLot, AnAppliedLotMovesTheLogRevision)
{
  SymbolRegistry registry;
  const SymbolId id = addSymbol(registry, "BTCUSDT", 0.0);
  const uint64_t rev = registry.venueTickLog()->revision();
  registry.applyVenueLot(id, Quantity::fromDouble(0.001));
  const uint64_t applied = registry.venueTickLog()->revision();
  EXPECT_GT(applied, rev);
  registry.applyVenueLot(id, Quantity::fromDouble(0.001));
  EXPECT_EQ(registry.venueTickLog()->revision(), applied) << "nothing changed, nothing to follow";
}

namespace
{

class Idle : public Strategy
{
 public:
  Idle(SymbolId sym, const SymbolRegistry& reg) : Strategy(1, sym, reg) {}
  void start() override {}
  void stop() override {}
};

SymbolId addUndeclared(SymbolRegistry& reg, const std::string& name)
{
  SymbolInfo info;
  info.exchange = "test";
  info.symbol = name;
  return reg.registerSymbol(info);
}

}  // namespace

// Strategy::symbolInfos() used to be the entries as copied in the
// constructor, whatever a connector applied afterwards.
TEST(StrategySymbolInfos, FollowAnAppliedTickAndLot)
{
  SymbolRegistry registry;
  const SymbolId id = addUndeclared(registry, "BTCUSDT");
  Idle strategy(id, registry);
  ASSERT_EQ(strategy.symbolInfos().size(), 1u);
  EXPECT_FALSE(strategy.symbolInfos()[0].tickSizeKnown);
  EXPECT_EQ(strategy.symbolInfos()[0].lotSize.raw(), 0);

  registry.applyVenueTick(id, Price::fromDouble(0.5));
  registry.applyVenueLot(id, Quantity::fromDouble(0.001));
  const auto infos = strategy.symbolInfos();
  EXPECT_TRUE(infos[0].tickSizeKnown);
  EXPECT_EQ(infos[0].tickSize.raw(), Price::fromDouble(0.5).raw());
  EXPECT_EQ(infos[0].lotSize.raw(), kMilliLot);
  EXPECT_EQ(orderTickRaw(infos[0]), Price::fromDouble(0.5).raw());
  EXPECT_EQ(orderLotRaw(infos[0]), kMilliLot);

  registry.applyVenueLot(id, Quantity::fromDouble(0.01));
  EXPECT_EQ(strategy.symbolInfos()[0].lotSize.raw(), Quantity::fromDouble(0.01).raw());
}

// The copy outlives the registry it was taken from, applied values included.
TEST(StrategySymbolInfos, OutliveTheRegistry)
{
  auto registry = std::make_unique<SymbolRegistry>();
  const SymbolId id = addUndeclared(*registry, "BTCUSDT");
  Idle strategy(id, *registry);
  registry->applyVenueLot(id, Quantity::fromDouble(0.001));
  registry.reset();
  EXPECT_EQ(strategy.symbolInfos()[0].lotSize.raw(), kMilliLot);
}

// A BacktestRunner that was never given a registry reads the strategy's
// entries. Values applied to the strategy's registry -- before or after the
// runner took the strategy, before or after its executor first read the
// symbol -- are the ones its executor checks against.
TEST(StrategySymbolInfos, ARunnerWithoutARegistryFollowsAppliedValues)
{
  SymbolRegistry registry;
  const SymbolId id = addUndeclared(registry, "BTCUSDT");
  Idle strategy(id, registry);

  BacktestRunner early;
  early.setStrategy(&strategy);
  ASSERT_EQ(early.executor().orderLotRaw(id), 0);
  ASSERT_EQ(early.executor().orderTickRaw(id), 0);

  registry.applyVenueTick(id, Price::fromDouble(0.5));
  registry.applyVenueLot(id, Quantity::fromDouble(0.001));
  EXPECT_EQ(early.executor().orderLotRaw(id), kMilliLot);
  EXPECT_EQ(early.executor().orderTickRaw(id), Price::fromDouble(0.5).raw());

  // A runner that takes the strategy after the values were applied.
  Idle second(id, registry);
  BacktestRunner late;
  late.setStrategy(&second);
  EXPECT_EQ(late.executor().orderLotRaw(id), kMilliLot);
  EXPECT_EQ(late.executor().orderTickRaw(id), Price::fromDouble(0.5).raw());

  // The venue moves both again.
  registry.applyVenueTick(id, Price::fromDouble(0.25));
  registry.applyVenueLot(id, Quantity::fromDouble(0.01));
  EXPECT_EQ(early.executor().orderLotRaw(id), Quantity::fromDouble(0.01).raw());
  EXPECT_EQ(late.executor().orderTickRaw(id), Price::fromDouble(0.25).raw());

  // A registry passed to the runner takes precedence, as before.
  SymbolRegistry own;
  SymbolInfo declared;
  declared.id = id;
  declared.exchange = "test";
  declared.symbol = "BTCUSDT";
  declared.lotSize = Quantity::fromDouble(0.1);
  own.registerSymbol(declared);
  early.setSymbolRegistry(&own);
  EXPECT_EQ(early.executor().orderLotRaw(id), Quantity::fromDouble(0.1).raw());
  early.setSymbolRegistry(nullptr);
  EXPECT_EQ(early.executor().orderLotRaw(id), Quantity::fromDouble(0.01).raw());
}
