/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// What a TOP_OF_BOOK frame does to the simulated venue. The frame is the
// venue's complete word on the touch and nothing deeper, so the touch
// moves, whatever the touch proves emptied is gone, and the rest of the
// ladder stays as the last depth frame reported it.

#include "lrvx/backtest/backtest_runner.h"
#include "lrvx/backtest/simulated_clock.h"
#include "lrvx/backtest/simulated_executor.h"
#include "lrvx/engine/symbol_registry.h"
#include "lrvx/replay/abstract_event_reader.h"
#include "lrvx/strategy/strategy.h"

#include <gtest/gtest.h>

#include <memory_resource>
#include <string>
#include <utility>
#include <vector>

using namespace lrvx;

namespace
{

constexpr SymbolId kSym = 1;

using Levels = std::vector<std::pair<double, double>>;

void pushBook(SimulatedExecutor& exec, SymbolId sym, bool top, const Levels& bids,
              const Levels& asks)
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
  if (top)
  {
    exec.onTopOfBook(sym, b, a);
  }
  else
  {
    exec.onBookUpdate(sym, b, a);
  }
}

void pushLadder(SimulatedExecutor& exec, SymbolId sym, const Levels& bids, const Levels& asks)
{
  pushBook(exec, sym, false, bids, asks);
}

void pushTop(SimulatedExecutor& exec, SymbolId sym, const Levels& bids, const Levels& asks)
{
  pushBook(exec, sym, true, bids, asks);
}

Order marketOrder(OrderId id, SymbolId sym, Side side, double qty)
{
  Order o;
  o.id = id;
  o.symbol = sym;
  o.side = side;
  o.type = OrderType::MARKET;
  o.quantity = Quantity::fromDouble(qty);
  return o;
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

}  // namespace

// Depth said the ask was 101; the top moved it to 100.50 and that is where
// the market buy prints, not at the stale depth best.
TEST(SimulatedExecutorTopOfBook, MarketBuyFillsAtTheTouchTheTopMoved)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);

  pushLadder(exec, kSym, {{99.0, 5.0}}, {{101.0, 5.0}, {102.0, 5.0}});
  pushTop(exec, kSym, {{99.5, 5.0}}, {{100.5, 2.0}});

  EXPECT_DOUBLE_EQ(exec.bestAskPrice(kSym).toDouble(), 100.5);
  EXPECT_DOUBLE_EQ(exec.bestBidPrice(kSym).toDouble(), 99.5);

  exec.submitOrder(marketOrder(1, kSym, Side::BUY, 1.0));
  ASSERT_EQ(exec.fills().size(), 1u);
  EXPECT_DOUBLE_EQ(exec.fills()[0].price.toDouble(), 100.5);
}

// The frame says nothing past the touch, so the depth behind it survives: a
// buy bigger than the new touch level walks 100.50, then the 101 and 102
// the last depth frame reported.
TEST(SimulatedExecutorTopOfBook, DeeperDepthSurvivesTheTop)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);

  pushLadder(exec, kSym, {{99.0, 5.0}}, {{101.0, 1.0}, {102.0, 5.0}});
  pushTop(exec, kSym, {{99.0, 5.0}}, {{100.5, 1.0}});

  exec.submitOrder(marketOrder(1, kSym, Side::BUY, 3.0));
  ASSERT_EQ(exec.fills().size(), 1u);
  // VWAP of 100.5 x 1 + 101 x 1 + 102 x 1, truncated to the fixed-point grid.
  EXPECT_NEAR(exec.fills()[0].price.toDouble(), (100.5 + 101.0 + 102.0) / 3.0, 1e-6);
}

// A touch that moved away proves the level it left emptied. The ask went
// 101 -> 102, so the 101 the depth frame reported is gone and the buy pays
// 102.
TEST(SimulatedExecutorTopOfBook, TouchRetreatDropsTheEmptiedLevel)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);

  pushLadder(exec, kSym, {{99.0, 5.0}}, {{101.0, 5.0}, {102.0, 5.0}});
  pushTop(exec, kSym, {{99.0, 5.0}}, {{102.0, 5.0}});

  exec.submitOrder(marketOrder(1, kSym, Side::BUY, 1.0));
  ASSERT_EQ(exec.fills().size(), 1u);
  EXPECT_DOUBLE_EQ(exec.fills()[0].price.toDouble(), 102.0);
}

// Same touch price, new size: the walk takes the frame's word for the level.
// Depth said 1 at 101; the top grew it to 5, so a buy of 5 prints entirely
// at 101 instead of spilling into 102.
TEST(SimulatedExecutorTopOfBook, TouchResizeGovernsTheWalk)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);

  pushLadder(exec, kSym, {{99.0, 5.0}}, {{101.0, 1.0}, {102.0, 10.0}});
  pushTop(exec, kSym, {{99.0, 5.0}}, {{101.0, 5.0}});

  exec.submitOrder(marketOrder(1, kSym, Side::BUY, 5.0));
  ASSERT_EQ(exec.fills().size(), 1u);
  EXPECT_DOUBLE_EQ(exec.fills()[0].price.toDouble(), 101.0);
}

// An empty side means that side of the book is empty -- the matrix contract.
// With no bid and no trade yet, a market sell has nothing to print against
// and rests instead of inventing a price.
TEST(SimulatedExecutorTopOfBook, EmptySideMeansEmptySide)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);

  pushLadder(exec, kSym, {{99.0, 5.0}}, {{101.0, 5.0}});
  pushTop(exec, kSym, {}, {{101.0, 5.0}});

  EXPECT_DOUBLE_EQ(exec.bestBidPrice(kSym).toDouble(), 0.0);

  exec.submitOrder(marketOrder(1, kSym, Side::SELL, 1.0));
  EXPECT_TRUE(exec.fills().empty());
}

// A top frame runs the same matching pass as a depth frame: the ask crossing
// down through a resting buy's price fills it, as a maker at the price it
// posted.
TEST(SimulatedExecutorTopOfBook, RestingLimitFillsWhenTheTopCrossesIt)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);

  pushLadder(exec, kSym, {{99.0, 5.0}}, {{102.0, 5.0}});
  exec.submitOrder(limitOrder(1, kSym, Side::BUY, 100.0, 1.0));
  ASSERT_TRUE(exec.fills().empty());

  pushTop(exec, kSym, {{98.0, 5.0}}, {{99.5, 5.0}});
  ASSERT_EQ(exec.fills().size(), 1u);
  EXPECT_DOUBLE_EQ(exec.fills()[0].price.toDouble(), 100.0);
  EXPECT_TRUE(exec.fills()[0].isMaker);
}

// The touch size is a level update for the queue tracker: the level our
// order queues at shrank from 10 to 4, so the queue ahead of us shrank with
// it and the executor says so.
TEST(SimulatedExecutorTopOfBook, QueueAheadFollowsTheTouchSize)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::FULL, 8);

  std::vector<std::pair<Quantity, Quantity>> queueEvents;  // (ahead, total)
  exec.setOrderEventCallback(
      [&](const OrderEvent& ev)
      {
        if (ev.status == OrderEventStatus::QUEUE_POSITION_UPDATED)
        {
          queueEvents.emplace_back(ev.queueAhead, ev.queueTotal);
        }
      });

  pushLadder(exec, kSym, {{100.0, 10.0}}, {{102.0, 5.0}});
  exec.submitOrder(limitOrder(1, kSym, Side::BUY, 100.0, 1.0));
  ASSERT_TRUE(exec.fills().empty());

  pushTop(exec, kSym, {{100.0, 4.0}}, {{102.0, 5.0}});
  ASSERT_FALSE(queueEvents.empty());
  EXPECT_LE(queueEvents.back().first.toDouble(), 4.0);
}

// A touch past our resting level proves everything at it traded or left:
// the bid stepped down to 99, so the 100 level we queued at is empty and we
// are at its front.
TEST(SimulatedExecutorTopOfBook, TopPastARestingLevelEmptiesItsQueue)
{
  SimulatedClock clock;
  SimulatedExecutor exec(clock);
  exec.setQueueModel(QueueModel::FULL, 8);

  std::vector<Quantity> aheads;
  exec.setOrderEventCallback(
      [&](const OrderEvent& ev)
      {
        if (ev.status == OrderEventStatus::QUEUE_POSITION_UPDATED)
        {
          aheads.push_back(ev.queueAhead);
        }
      });

  pushLadder(exec, kSym, {{100.0, 10.0}, {99.0, 5.0}}, {{102.0, 5.0}});
  exec.submitOrder(limitOrder(1, kSym, Side::BUY, 100.0, 1.0));

  pushTop(exec, kSym, {{99.0, 5.0}}, {{102.0, 5.0}});
  ASSERT_FALSE(aheads.empty());
  EXPECT_DOUBLE_EQ(aheads.back().toDouble(), 0.0);
}

// --- the runner wiring -----------------------------------------------------

namespace
{

SymbolId addSymbol(SymbolRegistry& reg, const std::string& spelling)
{
  SymbolInfo info;
  info.exchange = "test";
  info.symbol = spelling;
  info.type = InstrumentType::Spot;
  info.tickSize = Price::fromDouble(0.01);
  return reg.registerSymbol(info);
}

// In-memory reader, so run() can be driven without a tape.
class VectorReader : public replay::IMultiSegmentReader
{
 public:
  explicit VectorReader(std::vector<replay::ReplayEvent> events) : _events(std::move(events)) {}

  uint64_t forEach(EventCallback callback) override
  {
    uint64_t n = 0;
    for (const auto& ev : _events)
    {
      if (!callback(ev))
      {
        break;
      }
      ++n;
    }
    return n;
  }

  uint64_t forEachFrom(int64_t startNs, EventCallback callback) override
  {
    uint64_t n = 0;
    for (const auto& ev : _events)
    {
      if (ev.timestamp_ns < startNs)
      {
        continue;
      }
      if (!callback(ev))
      {
        break;
      }
      ++n;
    }
    return n;
  }

  const std::vector<replay::SegmentInfo>& segments() const override { return _segments; }
  uint64_t totalEvents() const override { return _events.size(); }

 private:
  std::vector<replay::ReplayEvent> _events;
  std::vector<replay::SegmentInfo> _segments;
};

replay::ReplayEvent bookEvent(replay::EventType type, SymbolId sym, int64_t tsNs,
                              const std::vector<std::pair<double, double>>& bids,
                              const std::vector<std::pair<double, double>>& asks)
{
  replay::ReplayEvent ev{};
  ev.type = type;
  ev.timestamp_ns = tsNs;
  ev.book_header.symbol_id = sym;
  ev.book_header.exchange_ts_ns = tsNs;
  for (const auto& [px, qty] : bids)
  {
    ev.bids.push_back({Price::fromDouble(px).raw(), Quantity::fromDouble(qty).raw()});
  }
  for (const auto& [px, qty] : asks)
  {
    ev.asks.push_back({Price::fromDouble(px).raw(), Quantity::fromDouble(qty).raw()});
  }
  return ev;
}

replay::ReplayEvent tradeEvent(SymbolId sym, int64_t tsNs, double price)
{
  replay::ReplayEvent ev{};
  ev.type = replay::EventType::Trade;
  ev.timestamp_ns = tsNs;
  ev.trade.symbol_id = sym;
  ev.trade.price_raw = Price::fromDouble(price).raw();
  ev.trade.qty_raw = Quantity::fromDouble(1.0).raw();
  ev.trade.side = 1;
  ev.trade.exchange_ts_ns = tsNs;
  return ev;
}

// Buys on the first trade, recording the ask touch its context showed. A
// TOP_OF_BOOK frame reaches the strategy as the context's touch fields, not
// as a book callback, so a trade is what triggers the order.
class TouchReaderBuyStrategy : public Strategy
{
 public:
  using Strategy::Strategy;

  SymbolId target{0};
  bool sent{false};
  double seenTouchAsk{0.0};

 protected:
  void onSymbolTrade(SymbolContext& ctx, const TradeEvent& /*ev*/) override
  {
    if (!sent && ctx.touchAsk.has_value())
    {
      sent = true;
      seenTouchAsk = ctx.touchAsk->price.toDouble();
      emitMarketBuy(target, Quantity::fromDouble(1.0));
    }
  }
};

}  // namespace

// End to end: depth says the ask is 101, a top frame moves the strategy's
// touch to 100.50, and the order it sends off that touch fills at the
// 100.50 it saw, not the stale 101.
TEST(SimulatedExecutorTopOfBook, RunnerFillsAtTheTouchTheStrategySaw)
{
  SymbolRegistry reg;
  const SymbolId sym = addSymbol(reg, "BTCUSDT");

  TouchReaderBuyStrategy strat(1, std::vector<SymbolId>{sym}, reg);
  strat.target = sym;

  BacktestRunner runner;
  runner.setStrategy(&strat);

  VectorReader reader({
      bookEvent(replay::EventType::BookSnapshot, sym, 1'000'000, {{99.0, 5.0}},
                {{101.0, 5.0}, {102.0, 5.0}}),
      bookEvent(replay::EventType::BookTop, sym, 2'000'000, {{99.5, 5.0}}, {{100.5, 2.0}}),
      tradeEvent(sym, 3'000'000, 100.0),
  });
  runner.run(reader);

  EXPECT_DOUBLE_EQ(strat.seenTouchAsk, 100.5);
  ASSERT_EQ(runner.executor().fills().size(), 1u);
  EXPECT_DOUBLE_EQ(runner.executor().fills()[0].price.toDouble(), 100.5);
}
