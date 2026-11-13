/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/backtest/backtest_config.h"
#include "lrvx/backtest/simulated_clock.h"
#include "lrvx/backtest/simulated_executor.h"

#include <gtest/gtest.h>

using namespace lrvx;

namespace
{
void pushBook(SimulatedExecutor& exec, SymbolId sym, double bid, double bidQty,
              double ask, double askQty)
{
  std::pmr::monotonic_buffer_resource pool(512);
  std::pmr::vector<BookLevel> bids(&pool);
  std::pmr::vector<BookLevel> asks(&pool);
  bids.emplace_back(Price::fromDouble(bid), Quantity::fromDouble(bidQty));
  asks.emplace_back(Price::fromDouble(ask), Quantity::fromDouble(askQty));
  exec.onBookUpdate(sym, bids, asks);
}

Order limitBuy(OrderId id, SymbolId sym, double price, double qty)
{
  Order o;
  o.id = id;
  o.symbol = sym;
  o.side = Side::BUY;
  o.type = OrderType::LIMIT;
  o.price = Price::fromDouble(price);
  o.quantity = Quantity::fromDouble(qty);
  return o;
}

BacktestConfig configWithReplaceLatency(int64_t latencyNs, int64_t jitterNs = 0)
{
  BacktestConfig cfg{};
  cfg.queueModel = QueueModel::TOB;
  cfg.queueDepth = 1;
  cfg.replaceAckLatencyNs = latencyNs;
  cfg.replaceAckJitterNs = jitterNs;
  cfg.cancelAckSeed = 42;
  return cfg;
}
}  // namespace

TEST(ReplaceInFlight, ZeroLatencyKeepsLegacyBehavior)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.applyConfig(configWithReplaceLatency(0));

  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(1, 1, 99.0, 1.0));
  Order replacement = limitBuy(1, 1, 99.5, 1.0);
  exec.replaceOrder(1, replacement);

  bool sawReplaced = false;
  bool sawReplaceSubmitted = false;
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::REPLACE_SUBMITTED)
    {
      sawReplaceSubmitted = true;
    }
    if (ev.status == OrderEventStatus::REPLACED)
    {
      sawReplaced = true;
    }
  }
  EXPECT_TRUE(sawReplaced);
  EXPECT_FALSE(sawReplaceSubmitted);
}

TEST(ReplaceInFlight, AsyncReplaceFiresSubmittedThenAcceptedAndReplaced)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.applyConfig(configWithReplaceLatency(10'000'000));  // 10 ms

  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(1, 1, 99.0, 1.0));
  Order replacement = limitBuy(1, 1, 99.5, 1.0);
  exec.replaceOrder(1, replacement);

  // Immediately: REPLACE_SUBMITTED only.
  bool sawSubmitted = false;
  bool sawAcceptedEarly = false;
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::REPLACE_SUBMITTED)
    {
      sawSubmitted = true;
    }
    if (ev.status == OrderEventStatus::REPLACE_ACCEPTED)
    {
      sawAcceptedEarly = true;
    }
  }
  EXPECT_TRUE(sawSubmitted);
  EXPECT_FALSE(sawAcceptedEarly);

  clock.advanceTo(clock.nowNs() + DurationNs{11'000'000});
  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);

  bool sawAccepted = false;
  bool sawReplaced = false;
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::REPLACE_ACCEPTED)
    {
      sawAccepted = true;
    }
    if (ev.status == OrderEventStatus::REPLACED)
    {
      sawReplaced = true;
    }
  }
  EXPECT_TRUE(sawAccepted);
  EXPECT_TRUE(sawReplaced);
}

TEST(ReplaceInFlight, LateReplaceAfterFillEmitsRejected)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.applyConfig(configWithReplaceLatency(10'000'000));

  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  pushBook(exec, 1, 100.0, 0.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(1, 1, 100.0, 1.0));
  Order replacement = limitBuy(1, 1, 99.5, 1.0);
  exec.replaceOrder(1, replacement);

  // Trade arrives within the 10 ms window and fills the original order.
  clock.advanceTo(clock.nowNs() + DurationNs{1'000'000});
  exec.onTrade(1, Price::fromDouble(100.0), Quantity::fromDouble(1.0), false);

  bool sawFilled = false;
  bool sawReject = false;
  std::string reason;
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::FILLED)
    {
      sawFilled = true;
    }
    if (ev.status == OrderEventStatus::REPLACE_REJECTED)
    {
      sawReject = true;
      reason = ev.rejectReason;
    }
  }
  EXPECT_TRUE(sawFilled);
  EXPECT_TRUE(sawReject);
  EXPECT_EQ(reason, "late_replace_after_fill");
}

TEST(ReplaceInFlight, AckCarriesNewOrderPayload)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.applyConfig(configWithReplaceLatency(5'000'000));

  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(1, 1, 99.0, 1.0));
  Order replacement = limitBuy(1, 1, 99.5, 2.0);
  exec.replaceOrder(1, replacement);

  clock.advanceTo(clock.nowNs() + DurationNs{6'000'000});
  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);

  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::REPLACE_ACCEPTED)
    {
      EXPECT_DOUBLE_EQ(ev.newOrder.price.toDouble(), 99.5);
      EXPECT_DOUBLE_EQ(ev.newOrder.quantity.toDouble(), 2.0);
    }
  }
}

// Nobody listening to order events changes nothing about what the simulator
// does with a pending replacement: pending replaces were finalized only when
// an order-event callback was set, so without one the replacement was held
// forever and the original kept working at its old price and size.
TEST(ReplaceInFlight, PendingReplaceAppliesWithNoListener)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.applyConfig(configWithReplaceLatency(5'000'000));

  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(1, 1, 99.0, 1.0));
  exec.replaceOrder(1, limitBuy(1, 1, 99.5, 2.0));
  ASSERT_NE(exec.findOrder(1), nullptr);
  EXPECT_DOUBLE_EQ(exec.findOrder(1)->price.toDouble(), 99.0);

  clock.advanceTo(clock.nowNs() + DurationNs{6'000'000});
  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);

  const Order* now = exec.findOrder(1);
  ASSERT_NE(now, nullptr);
  EXPECT_DOUBLE_EQ(now->price.toDouble(), 99.5);
  EXPECT_DOUBLE_EQ(now->quantity.toDouble(), 2.0);
}

// A replacement still pending when the original fills is dropped with or
// without a listener: one left behind landed on a later order that reused
// the id.
TEST(ReplaceInFlight, LateReplaceAfterFillIsDroppedWithNoListener)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.applyConfig(configWithReplaceLatency(10'000'000));

  pushBook(exec, 1, 100.0, 0.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(1, 1, 100.0, 1.0));
  exec.replaceOrder(1, limitBuy(1, 1, 99.5, 3.0));
  clock.advanceTo(clock.nowNs() + DurationNs{1'000'000});
  exec.onTrade(1, Price::fromDouble(100.0), Quantity::fromDouble(1.0), false);
  ASSERT_EQ(exec.findOrder(1), nullptr);

  exec.submitOrder(limitBuy(1, 1, 98.0, 1.0));
  clock.advanceTo(clock.nowNs() + DurationNs{20'000'000});
  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);
  const Order* now = exec.findOrder(1);
  ASSERT_NE(now, nullptr);
  EXPECT_DOUBLE_EQ(now->price.toDouble(), 98.0);
  EXPECT_DOUBLE_EQ(now->quantity.toDouble(), 1.0);
}
