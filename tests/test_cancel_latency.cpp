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

#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

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

BacktestConfig configWithCancelLatency(int64_t latencyNs, int64_t jitterNs = 0)
{
  BacktestConfig cfg{};
  cfg.queueModel = QueueModel::TOB;
  cfg.queueDepth = 1;
  cfg.cancelAckLatencyNs = latencyNs;
  cfg.cancelAckJitterNs = jitterNs;
  cfg.cancelAckSeed = 42;
  return cfg;
}
}  // namespace

TEST(CancelLatency, ZeroLatencyKeepsSynchronousBehavior)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.applyConfig(configWithCancelLatency(0));

  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(1, 1, 99.0, 1.0));
  exec.cancelOrder(1);

  bool sawCanceled = false;
  bool sawPending = false;
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::PENDING_CANCEL)
    {
      sawPending = true;
    }
    if (ev.status == OrderEventStatus::CANCELED)
    {
      sawCanceled = true;
    }
  }
  EXPECT_TRUE(sawCanceled);
  EXPECT_FALSE(sawPending);
}

TEST(CancelLatency, AsyncCancelEmitsPendingThenCanceledAfterAck)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.applyConfig(configWithCancelLatency(10'000'000));  // 10 ms

  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(1, 1, 99.0, 1.0));
  exec.cancelOrder(1);

  // Immediately after cancelOrder: PENDING_CANCEL fired, CANCELED has not.
  bool sawPending = false;
  bool sawCanceledEarly = false;
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::PENDING_CANCEL)
    {
      sawPending = true;
    }
    if (ev.status == OrderEventStatus::CANCELED)
    {
      sawCanceledEarly = true;
    }
  }
  EXPECT_TRUE(sawPending);
  EXPECT_FALSE(sawCanceledEarly);

  // Advance the clock past the ack deadline and feed a no-op book update
  // to give the simulator a tick.
  clock.advanceTo(clock.nowNs() + DurationNs{11'000'000});
  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);

  bool sawCanceledAfter = false;
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::CANCELED)
    {
      sawCanceledAfter = true;
    }
  }
  EXPECT_TRUE(sawCanceledAfter);
}

TEST(CancelLatency, LateCancelAfterFillEmitsRejected)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.applyConfig(configWithCancelLatency(10'000'000));

  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  pushBook(exec, 1, 100.0, 0.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(1, 1, 100.0, 1.0));
  exec.cancelOrder(1);

  // Aggressive trade arrives within the 10 ms ack window and fills the
  // resting order.
  clock.advanceTo(clock.nowNs() + DurationNs{1'000'000});  // 1 ms in
  exec.onTrade(1, Price::fromDouble(100.0), Quantity::fromDouble(1.0), false);

  bool sawFilled = false;
  bool sawRejected = false;
  std::string rejectReason;
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::FILLED)
    {
      sawFilled = true;
    }
    if (ev.status == OrderEventStatus::REJECTED)
    {
      sawRejected = true;
      rejectReason = ev.rejectReason;
    }
  }
  EXPECT_TRUE(sawFilled);
  EXPECT_TRUE(sawRejected);
  EXPECT_EQ(rejectReason, "late_cancel_after_fill");
}

TEST(CancelLatency, AckDeferredBelowBandFinalizedAbove)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.applyConfig(configWithCancelLatency(10'000'000, 2'000'000));

  std::vector<OrderEvent> events;
  exec.setOrderEventCallback([&events](const OrderEvent& ev)
                             { events.push_back(ev); });

  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(1, 1, 99.0, 1.0));
  exec.cancelOrder(1);

  // 7 ms is strictly below `base - jitter` (= 8 ms). No CANCELED yet.
  clock.advanceTo(clock.nowNs() + DurationNs{7'000'000});
  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);
  for (const auto& ev : events)
  {
    EXPECT_NE(ev.status, OrderEventStatus::CANCELED);
  }

  // 13 ms total is strictly above `base + jitter` (= 12 ms). Now the
  // ack must have fired.
  clock.advanceTo(clock.nowNs() + DurationNs{6'000'000});
  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);
  bool sawCanceled = false;
  for (const auto& ev : events)
  {
    if (ev.status == OrderEventStatus::CANCELED)
    {
      sawCanceled = true;
    }
  }
  EXPECT_TRUE(sawCanceled);
}

namespace
{
using EventTrail = std::vector<std::pair<OrderEventStatus, OrderId>>;

Order stopSell(OrderId id, SymbolId sym, double trigger, double qty)
{
  Order o;
  o.id = id;
  o.symbol = sym;
  o.side = Side::SELL;
  o.type = OrderType::STOP_MARKET;
  o.triggerPrice = Price::fromDouble(trigger);
  o.quantity = Quantity::fromDouble(qty);
  return o;
}

// One order of every kind cancel-all reaches on symbol 1 (held for the next
// bar, resting, conditional), plus a resting order on symbol 2 it must leave
// alone. `cancel` runs once the book is set up; the trail records every event
// from that point through an ack window in which the book trades through
// order 1. A NEW event with id 0 marks the return of the cancel call.
EventTrail runCancelScenario(int64_t cancelLatencyNs,
                             const std::function<void(SimulatedExecutor&)>& cancel)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.applyConfig(configWithCancelLatency(cancelLatencyNs, cancelLatencyNs / 5));

  EventTrail trail;
  bool recording = false;
  exec.setOrderEventCallback(
      [&](const OrderEvent& ev)
      {
        if (recording && ev.status != OrderEventStatus::MARKET_POSITION_CHANGED &&
            ev.status != OrderEventStatus::QUEUE_POSITION_UPDATED)
        {
          trail.emplace_back(ev.status, ev.order.id);
        }
      });

  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);
  pushBook(exec, 2, 100.0, 5.0, 101.0, 5.0);
  exec.submitOrder(limitBuy(1, 1, 99.0, 1.0));
  exec.submitOrder(limitBuy(2, 1, 98.0, 1.0));
  exec.submitOrder(limitBuy(5, 2, 98.0, 1.0));
  exec.submitOrder(limitBuy(4, 1, 97.0, 1.0));
  exec.submitOrder(stopSell(3, 1, 90.0, 1.0));
  {
    SimulatedExecutor::BarCallbackScope window(exec);
    exec.submitOrder(limitBuy(6, 1, 96.0, 1.0));
  }

  recording = true;
  cancel(exec);
  trail.emplace_back(OrderEventStatus::NEW, 0);

  clock.advanceTo(clock.nowNs() + DurationNs{1'000'000});
  exec.onTrade(1, Price::fromDouble(99.0), Quantity::fromDouble(5.0), false);
  clock.advanceTo(clock.nowNs() + DurationNs{50'000'000});
  pushBook(exec, 1, 100.0, 5.0, 101.0, 5.0);
  pushBook(exec, 2, 100.0, 5.0, 101.0, 5.0);
  return trail;
}

EventTrail trailBeforeReturn(const EventTrail& trail)
{
  return EventTrail(trail.begin(),
                    std::find(trail.begin(), trail.end(),
                              std::make_pair(OrderEventStatus::NEW, OrderId{0})));
}
}  // namespace

// Pins cancel-all without an ack latency: one synchronous sweep, every order
// of the symbol announced CANCELED before the call returns -- held orders
// first, then resting ones, then conditional ones -- and nothing on symbol 2.
TEST(CancelLatency, CancelAllZeroLatencyIsSynchronousSweep)
{
  const EventTrail trail =
      runCancelScenario(0, [](SimulatedExecutor& e)
                        { e.cancelAllOrders(1); });
  const EventTrail expected{{OrderEventStatus::CANCELED, 6},
                            {OrderEventStatus::CANCELED, 1},
                            {OrderEventStatus::CANCELED, 4},
                            {OrderEventStatus::CANCELED, 2},
                            {OrderEventStatus::CANCELED, 3},
                            {OrderEventStatus::NEW, 0}};
  EXPECT_EQ(trail, expected);
}

// With an ack latency cancel-all is the same request as cancelling every
// order of the symbol one by one: held orders cancel at once (they never
// reached the venue), the rest go PENDING_CANCEL, stay live through the ack
// window, and a fill inside it rejects the late cancel.
TEST(CancelLatency, CancelAllHonoursAckLatencyLikeIndividualCancels)
{
  const EventTrail all =
      runCancelScenario(10'000'000, [](SimulatedExecutor& e)
                        { e.cancelAllOrders(1); });
  const EventTrail each = runCancelScenario(10'000'000,
                                            [](SimulatedExecutor& e)
                                            {
                                              for (OrderId id : {6, 1, 4, 2, 3})
                                              {
                                                e.cancelOrder(id);
                                              }
                                            });
  EXPECT_EQ(all, each);

  const EventTrail expectedBeforeReturn{{OrderEventStatus::CANCELED, 6},
                                        {OrderEventStatus::PENDING_CANCEL, 1},
                                        {OrderEventStatus::PENDING_CANCEL, 4},
                                        {OrderEventStatus::PENDING_CANCEL, 2},
                                        {OrderEventStatus::PENDING_CANCEL, 3}};
  EXPECT_EQ(trailBeforeReturn(all), expectedBeforeReturn);

  const auto has = [&](OrderEventStatus s, OrderId id)
  { return std::find(all.begin(), all.end(), std::make_pair(s, id)) != all.end(); };
  EXPECT_TRUE(has(OrderEventStatus::FILLED, 1));
  EXPECT_TRUE(has(OrderEventStatus::REJECTED, 1));
  EXPECT_FALSE(has(OrderEventStatus::CANCELED, 1));
  EXPECT_TRUE(has(OrderEventStatus::CANCELED, 2));
  EXPECT_TRUE(has(OrderEventStatus::CANCELED, 3));
  EXPECT_TRUE(has(OrderEventStatus::CANCELED, 4));
  for (const auto& entry : all)
  {
    EXPECT_NE(entry.second, OrderId{5}) << "cancel-all on symbol 1 touched symbol 2";
  }
}
