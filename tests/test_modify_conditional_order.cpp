/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// A modify of an order that is still waiting on its trigger changes that
// order: a stop, take-profit or trailing order gets the new price and
// quantity and keeps its id, side and type. A modify that names no live
// order is refused with a REPLACE_REJECTED event instead of vanishing.

#include "lrvx/backtest/backtest_config.h"
#include "lrvx/backtest/backtest_runner.h"
#include "lrvx/backtest/simulated_clock.h"
#include "lrvx/backtest/simulated_executor.h"
#include "lrvx/engine/symbol_registry.h"
#include "lrvx/strategy/strategy.h"

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <string>
#include <utility>
#include <vector>

using namespace lrvx;

namespace
{

constexpr int64_t kMinuteNs = 60'000'000'000LL;
constexpr SymbolId kSym = 1;

Price px(double v)
{
  return Price::fromDouble(v);
}
Quantity qty(double v)
{
  return Quantity::fromDouble(v);
}

SymbolId addSymbol(SymbolRegistry& reg, const std::string& name)
{
  SymbolInfo info;
  info.exchange = "test";
  info.symbol = name;
  info.type = InstrumentType::Spot;
  info.tickSize = px(0.5);
  return reg.registerSymbol(info);
}

BarEvent makeBar(SymbolId sym, size_t index, double high, double low)
{
  const int64_t startNs = 1'700'000'000'000'000'000LL + static_cast<int64_t>(index) * kMinuteNs;
  BarEvent ev{};
  ev.symbol = sym;
  ev.barType = BarType::Time;
  ev.barTypeParam = static_cast<uint64_t>(kMinuteNs);
  ev.bar.open = px(100.0);
  ev.bar.high = px(high);
  ev.bar.low = px(low);
  ev.bar.close = px(100.0);
  ev.bar.volume = Volume::fromDouble(10.0);
  ev.bar.startTime = TimePoint{std::chrono::nanoseconds{startNs}};
  ev.bar.endTime = TimePoint{std::chrono::nanoseconds{startNs + kMinuteNs}};
  return ev;
}

// Six bars around 100. Only the fifth leaves the 99.5 .. 100.5 band.
std::vector<BarEvent> makeBars(SymbolId sym, double spikeHigh, double spikeLow)
{
  std::vector<BarEvent> bars;
  for (size_t i = 0; i < 6; ++i)
  {
    const bool spike = i == 4;
    bars.push_back(makeBar(sym, i, spike ? spikeHigh : 100.5, spike ? spikeLow : 99.5));
  }
  return bars;
}

// Runs `place` on the first bar and `modify` on the third, and records every
// order event the runner hands back.
class Scripted : public Strategy
{
 public:
  using Step = std::function<void(Scripted&)>;

  Scripted(SymbolId sym, const SymbolRegistry& reg, Step place, Step modify)
      : Strategy(1, std::vector<SymbolId>{sym}, reg),
        sym(sym),
        _place(std::move(place)),
        _modify(std::move(modify))
  {
  }

  SymbolId sym;
  OrderId orderId{0};
  std::vector<OrderEvent> fills;
  std::vector<OrderEvent> updates;

  using Strategy::emitCancel;
  using Strategy::emitMarketBuy;
  using Strategy::emitModify;
  using Strategy::emitStopLimit;
  using Strategy::emitStopMarket;
  using Strategy::emitTakeProfitMarket;
  using Strategy::emitTrailingStop;

  std::vector<OrderEvent> withStatus(OrderEventStatus status) const
  {
    std::vector<OrderEvent> out;
    for (const OrderEvent& ev : updates)
    {
      if (ev.status == status)
      {
        out.push_back(ev);
      }
    }
    return out;
  }

 protected:
  void onSymbolBar(SymbolContext& /*ctx*/, const BarEvent& /*ev*/) override
  {
    ++_seen;
    if (_seen == 1 && _place)
    {
      _place(*this);
    }
    else if (_seen == 3 && _modify)
    {
      _modify(*this);
    }
  }
  void onSymbolFill(SymbolContext& /*ctx*/, const OrderEvent& ev) override
  {
    fills.push_back(ev);
  }
  void onSymbolOrderUpdate(SymbolContext& /*ctx*/, const OrderEvent& ev) override
  {
    updates.push_back(ev);
  }

 private:
  Step _place;
  Step _modify;
  size_t _seen{0};
};

struct Fixture
{
  SymbolRegistry reg;
  SymbolId sym{addSymbol(reg, "BTCUSDT")};

  void run(Scripted& strat, double spikeHigh, double spikeLow, const BacktestConfig& cfg = {})
  {
    BacktestRunner runner(cfg);
    runner.setSymbolRegistry(&reg);
    runner.setStrategy(&strat);
    runner.runBars(makeBars(sym, spikeHigh, spikeLow));
  }
};

Order conditional(OrderId id, OrderType type, Side side, double trigger, double quantity)
{
  Order o;
  o.id = id;
  o.symbol = kSym;
  o.side = side;
  o.type = type;
  o.triggerPrice = px(trigger);
  o.quantity = qty(quantity);
  return o;
}

}  // namespace

TEST(ModifyConditionalOrder, StopMarketTakesTheNewTriggerAndQuantity)
{
  auto place = [](Scripted& s)
  { s.orderId = s.emitStopMarket(s.sym, Side::SELL, px(95.0), qty(1.0)); };

  // As placed the stop sits at 95 and a low of 97 never reaches it.
  {
    Fixture r;
    Scripted strat(r.sym, r.reg, place, nullptr);
    r.run(strat, 100.5, 97.0);
    EXPECT_TRUE(strat.fills.empty());
  }

  Fixture r;
  Scripted strat(r.sym, r.reg, place,
                 [](Scripted& s)
                 { s.emitModify(s.orderId, px(98.0), qty(2.0)); });
  r.run(strat, 100.5, 97.0);

  const auto replaced = strat.withStatus(OrderEventStatus::REPLACED);
  ASSERT_EQ(replaced.size(), 1u);
  EXPECT_EQ(replaced[0].order.id, strat.orderId);
  EXPECT_EQ(replaced[0].order.triggerPrice.raw(), px(95.0).raw());
  EXPECT_EQ(replaced[0].newOrder.id, strat.orderId);
  EXPECT_EQ(replaced[0].newOrder.type, OrderType::STOP_MARKET);
  EXPECT_EQ(replaced[0].newOrder.side, Side::SELL);
  EXPECT_EQ(replaced[0].newOrder.symbol, r.sym);
  EXPECT_EQ(replaced[0].newOrder.triggerPrice.raw(), px(98.0).raw());
  EXPECT_EQ(replaced[0].newOrder.quantity.raw(), qty(2.0).raw());

  const auto triggered = strat.withStatus(OrderEventStatus::TRIGGERED);
  ASSERT_EQ(triggered.size(), 1u);
  EXPECT_EQ(triggered[0].order.id, strat.orderId);
  EXPECT_EQ(triggered[0].order.triggerPrice.raw(), px(98.0).raw());

  ASSERT_EQ(strat.fills.size(), 1u);
  EXPECT_EQ(strat.fills[0].order.id, strat.orderId);
  EXPECT_EQ(strat.fills[0].order.side, Side::SELL);
  EXPECT_EQ(strat.fills[0].fillQty.raw(), qty(2.0).raw());
}

TEST(ModifyConditionalOrder, TakeProfitMarketTakesTheNewTrigger)
{
  Fixture r;
  Scripted strat(
      r.sym, r.reg,
      [](Scripted& s)
      { s.orderId = s.emitTakeProfitMarket(s.sym, Side::SELL, px(110.0), qty(1.0)); },
      [](Scripted& s)
      { s.emitModify(s.orderId, px(102.0), qty(0.5)); });
  r.run(strat, 103.0, 99.5);

  ASSERT_EQ(strat.withStatus(OrderEventStatus::REPLACED).size(), 1u);
  ASSERT_EQ(strat.fills.size(), 1u);
  EXPECT_EQ(strat.fills[0].order.id, strat.orderId);
  EXPECT_EQ(strat.fills[0].fillQty.raw(), qty(0.5).raw());
}

TEST(ModifyConditionalOrder, StopLimitTakesTheNewLimitAndKeepsItsTrigger)
{
  Fixture r;
  Scripted strat(
      r.sym, r.reg,
      [](Scripted& s)
      { s.orderId = s.emitStopLimit(s.sym, Side::SELL, px(98.0), px(90.0), qty(1.0)); },
      [](Scripted& s)
      { s.emitModify(s.orderId, px(97.5), qty(2.0)); });
  r.run(strat, 100.5, 99.5);

  const auto replaced = strat.withStatus(OrderEventStatus::REPLACED);
  ASSERT_EQ(replaced.size(), 1u);
  EXPECT_EQ(replaced[0].newOrder.type, OrderType::STOP_LIMIT);
  EXPECT_EQ(replaced[0].newOrder.triggerPrice.raw(), px(98.0).raw());
  EXPECT_EQ(replaced[0].newOrder.price.raw(), px(97.5).raw());
  EXPECT_EQ(replaced[0].newOrder.quantity.raw(), qty(2.0).raw());
  EXPECT_TRUE(strat.fills.empty()) << "the trigger at 98 was never reached";
}

TEST(ModifyConditionalOrder, TrailingStopTakesTheNewTrigger)
{
  // A 5.0 trail under a market at 100 puts the trigger near 95: a low of 99
  // is far from it. Moved to 99.5 by the modify, the same low triggers it.
  auto place = [](Scripted& s)
  { s.orderId = s.emitTrailingStop(s.sym, Side::SELL, px(5.0), qty(1.0)); };
  {
    Fixture r;
    Scripted strat(r.sym, r.reg, place, nullptr);
    r.run(strat, 100.5, 99.0);
    EXPECT_TRUE(strat.fills.empty());
  }

  Fixture r;
  Scripted strat(r.sym, r.reg, place,
                 [](Scripted& s)
                 { s.emitModify(s.orderId, px(99.5), qty(2.0)); });
  r.run(strat, 100.5, 99.0);

  const auto replaced = strat.withStatus(OrderEventStatus::REPLACED);
  ASSERT_EQ(replaced.size(), 1u);
  EXPECT_EQ(replaced[0].newOrder.type, OrderType::TRAILING_STOP);
  EXPECT_EQ(replaced[0].newOrder.trailingOffset.raw(), px(5.0).raw());
  ASSERT_EQ(strat.fills.size(), 1u);
  EXPECT_EQ(strat.fills[0].order.id, strat.orderId);
  EXPECT_EQ(strat.fills[0].fillQty.raw(), qty(2.0).raw());
}

TEST(ModifyConditionalOrder, OffTickTriggerIsRefusedAndTheOrderIsLeftAlone)
{
  Fixture r;
  Scripted strat(
      r.sym, r.reg,
      [](Scripted& s)
      { s.orderId = s.emitStopMarket(s.sym, Side::SELL, px(98.0), qty(1.0)); },
      [](Scripted& s)
      { s.emitModify(s.orderId, px(99.25), qty(2.0)); });
  r.run(strat, 100.5, 97.0);

  const auto rejects = strat.withStatus(OrderEventStatus::REPLACE_REJECTED);
  ASSERT_EQ(rejects.size(), 1u);
  EXPECT_EQ(rejects[0].rejectReason, "trigger_off_tick");
  EXPECT_EQ(rejects[0].order.id, strat.orderId);
  EXPECT_TRUE(strat.withStatus(OrderEventStatus::REPLACED).empty());
  ASSERT_EQ(strat.fills.size(), 1u) << "the original stop at 98 still triggers";
  EXPECT_EQ(strat.fills[0].fillQty.raw(), qty(1.0).raw());
}

TEST(ModifyConditionalOrder, ModifiedOrderCancelsByTheSameId)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });
  exec.onTrade(kSym, px(100.0), true);
  exec.submitOrder(conditional(7, OrderType::STOP_MARKET, Side::SELL, 95.0, 1.0));
  exec.replaceOrder(7, conditional(7, OrderType::STOP_MARKET, Side::SELL, 98.0, 2.0));

  ASSERT_EQ(exec.conditionalOrders().size(), 1u);
  EXPECT_EQ(exec.conditionalOrders()[0].triggerPrice.raw(), px(98.0).raw());
  EXPECT_EQ(exec.conditionalOrders()[0].quantity.raw(), qty(2.0).raw());

  exec.cancelOrder(7);
  EXPECT_TRUE(exec.conditionalOrders().empty());
  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events.back().status, OrderEventStatus::CANCELED);
  EXPECT_EQ(events.back().order.id, 7u);
}

TEST(ModifyConditionalOrder, UnknownOrderIsRefused)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });
  exec.onTrade(kSym, px(100.0), true);

  Order replacement;
  replacement.id = 42;
  replacement.symbol = kSym;
  replacement.price = px(99.0);
  replacement.quantity = qty(1.0);
  exec.replaceOrder(42, replacement);

  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].status, OrderEventStatus::REPLACE_REJECTED);
  EXPECT_EQ(events[0].rejectReason, "unknown_order");
  EXPECT_EQ(events[0].order.id, 42u);
  EXPECT_EQ(events[0].newOrder.id, 42u);
}

TEST(ModifyConditionalOrder, UnknownOrderIsRefusedWithReplaceLatencyToo)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  BacktestConfig cfg{};
  cfg.replaceAckLatencyNs = 1'000'000;
  exec.applyConfig(cfg);
  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });
  exec.onTrade(kSym, px(100.0), true);

  Order replacement;
  replacement.id = 42;
  replacement.symbol = kSym;
  replacement.price = px(99.0);
  replacement.quantity = qty(1.0);
  exec.replaceOrder(42, replacement);

  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].status, OrderEventStatus::REPLACE_REJECTED);
  EXPECT_EQ(events[0].rejectReason, "unknown_order");
}

TEST(ModifyConditionalOrder, FinishedOrderIsRefusedAndTheStrategyHearsIt)
{
  Fixture r;
  Scripted strat(
      r.sym, r.reg,
      [](Scripted& s)
      { s.orderId = s.emitMarketBuy(s.sym, qty(1.0)); },
      [](Scripted& s)
      { s.emitModify(s.orderId, px(99.0), qty(2.0)); });
  r.run(strat, 100.5, 99.5);

  ASSERT_EQ(strat.fills.size(), 1u) << "the market buy filled before the modify";
  const auto rejects = strat.withStatus(OrderEventStatus::REPLACE_REJECTED);
  ASSERT_EQ(rejects.size(), 1u);
  EXPECT_EQ(rejects[0].rejectReason, "unknown_order");
  EXPECT_EQ(rejects[0].order.id, strat.orderId);
  EXPECT_EQ(rejects[0].order.symbol, r.sym);
}

TEST(ModifyConditionalOrder, ReplaceLatencyDefersTheChange)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  BacktestConfig cfg{};
  cfg.replaceAckLatencyNs = 1'000'000;
  exec.applyConfig(cfg);
  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  clock.advanceTo(UnixNanos::fromRaw(1'000'000'000));
  exec.onTrade(kSym, px(100.0), true);
  exec.submitOrder(conditional(7, OrderType::STOP_MARKET, Side::SELL, 95.0, 1.0));
  exec.replaceOrder(7, conditional(7, OrderType::STOP_MARKET, Side::SELL, 98.0, 2.0));

  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events.back().status, OrderEventStatus::REPLACE_SUBMITTED);
  ASSERT_EQ(exec.conditionalOrders().size(), 1u);
  EXPECT_EQ(exec.conditionalOrders()[0].triggerPrice.raw(), px(95.0).raw())
      << "the original stays in force until the ack";

  clock.advanceTo(UnixNanos::fromRaw(1'002'000'000));
  exec.onTrade(kSym, px(100.0), true);

  ASSERT_EQ(exec.conditionalOrders().size(), 1u);
  EXPECT_EQ(exec.conditionalOrders()[0].triggerPrice.raw(), px(98.0).raw());
  EXPECT_EQ(exec.conditionalOrders()[0].quantity.raw(), qty(2.0).raw());
  bool replaced = false;
  for (const OrderEvent& ev : events)
  {
    replaced = replaced || ev.status == OrderEventStatus::REPLACED;
  }
  EXPECT_TRUE(replaced);
}

TEST(ModifyConditionalOrder, TriggerInsideTheReplaceWindowRefusesTheReplace)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  BacktestConfig cfg{};
  cfg.replaceAckLatencyNs = 1'000'000;
  exec.applyConfig(cfg);
  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  clock.advanceTo(UnixNanos::fromRaw(1'000'000'000));
  exec.onTrade(kSym, px(100.0), true);
  // Once triggered the original rests far above the market, so it does not
  // fill and the replace has a live order to be late for.
  Order original = conditional(7, OrderType::STOP_LIMIT, Side::SELL, 99.0, 1.0);
  original.price = px(150.0);
  exec.submitOrder(original);
  Order replacement = conditional(7, OrderType::STOP_LIMIT, Side::SELL, 99.0, 2.0);
  replacement.price = px(90.0);
  exec.replaceOrder(7, replacement);

  // The stop triggers before the replace is acknowledged.
  clock.advanceTo(UnixNanos::fromRaw(1'000'500'000));
  exec.onTrade(kSym, px(98.5), false);
  EXPECT_TRUE(exec.conditionalOrders().empty());

  clock.advanceTo(UnixNanos::fromRaw(1'002'000'000));
  exec.onTrade(kSym, px(98.5), false);

  std::vector<std::string> rejectReasons;
  bool replaced = false;
  for (const OrderEvent& ev : events)
  {
    if (ev.status == OrderEventStatus::REPLACE_REJECTED)
    {
      rejectReasons.push_back(ev.rejectReason);
    }
    replaced = replaced || ev.status == OrderEventStatus::REPLACED;
  }
  EXPECT_FALSE(replaced);
  ASSERT_EQ(rejectReasons.size(), 1u);
  EXPECT_EQ(rejectReasons[0], "late_replace_after_trigger");
  const Order* live = exec.findOrder(7);
  ASSERT_NE(live, nullptr);
  EXPECT_EQ(live->quantity.raw(), qty(1.0).raw()) << "the triggered order is as it was placed";
}
