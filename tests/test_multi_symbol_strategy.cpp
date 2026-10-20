/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/book/events/book_update_event.h"
#include "lrvx/book/events/trade_event.h"
#include "lrvx/engine/symbol_registry.h"
#include "lrvx/position/position_tracker.h"
#include "lrvx/strategy/strategy.h"
#include "lrvx/strategy/symbol_context.h"
#include "lrvx/strategy/symbol_state_map.h"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <memory_resource>
#include <thread>
#include <vector>

using namespace lrvx;

namespace
{
void populateRegistry(SymbolRegistry& registry, const std::vector<SymbolId>& symbols)
{
  for (SymbolId sym : symbols)
  {
    SymbolInfo info;
    info.exchange = "TEST";
    info.symbol = "SYM" + std::to_string(sym);
    info.tickSize = Price::fromDouble(0.01);
    registry.registerSymbol(info);
  }
}
}  // namespace

class SymbolStateMapTest : public ::testing::Test
{
};

TEST_F(SymbolStateMapTest, FlatArrayAccess)
{
  struct TestState
  {
    int value{0};
  };
  SymbolStateMap<TestState> map;

  map[5].value = 42;
  EXPECT_EQ(map[5].value, 42);
  EXPECT_TRUE(map.contains(5));
  EXPECT_FALSE(map.contains(6));
}

TEST_F(SymbolStateMapTest, OverflowAccess)
{
  struct TestState
  {
    int value{0};
  };
  SymbolStateMap<TestState, 10> map;

  map[500].value = 99;
  EXPECT_EQ(map[500].value, 99);
  EXPECT_TRUE(map.contains(500));
}

TEST_F(SymbolStateMapTest, TryGetReturnsNullptrForMissing)
{
  struct TestState
  {
    int value{0};
  };
  SymbolStateMap<TestState> map;

  EXPECT_EQ(map.tryGet(5), nullptr);
  map[5].value = 10;
  EXPECT_NE(map.tryGet(5), nullptr);
  EXPECT_EQ(map.tryGet(5)->value, 10);
}

TEST_F(SymbolStateMapTest, ForEachIteratesAll)
{
  struct TestState
  {
    int value{0};
  };
  SymbolStateMap<TestState, 10> map;

  map[1].value = 1;
  map[2].value = 2;
  map[300].value = 300;

  int sum = 0;
  map.forEach([&sum](SymbolId, const TestState& s)
              { sum += s.value; });
  EXPECT_EQ(sum, 303);
}

TEST_F(SymbolStateMapTest, ClearResetsAll)
{
  struct TestState
  {
    int value{0};
  };
  SymbolStateMap<TestState> map;

  map[1].value = 100;
  map[2].value = 200;
  EXPECT_EQ(map.size(), 2);

  map.clear();
  EXPECT_EQ(map.size(), 0);
  EXPECT_FALSE(map.contains(1));
}

// finding 4 behind the SymbolStateMap fix: getOverflow() used to emplace_back into a std::vector,
// which reallocates and moves every existing element once capacity runs out
// -- invalidating every reference operator[]/tryGet/an iterator had already
// handed out. `State& a = map[300];` followed by enough further overflow
// inserts left `a` pointing at freed memory. The overflow container is now a
// std::deque, whose push/emplace at either end never relocates existing
// elements, so this checks the address itself stays put across a lot of
// growth -- not just that the value read back is still right (a linear scan
// by symbol id would report the right value from either container; it is
// the pointer identity that the vector broke).
TEST_F(SymbolStateMapTest, OverflowReferencesStableAcrossGrowth)
{
  struct TestState
  {
    int value{0};
  };
  SymbolStateMap<TestState, 10> map;

  map[300].value = 111;
  const auto addrBefore = reinterpret_cast<std::uintptr_t>(&map[300]);

  // Comfortably past any small_vector-style initial capacity a std::vector
  // implementation might pick; std::vector would have reallocated several
  // times over this many push_backs.
  for (SymbolId sym = 301; sym < 400; ++sym)
  {
    map[sym].value = static_cast<int>(sym);
  }

  const auto addrAfter = reinterpret_cast<std::uintptr_t>(&map[300]);
  EXPECT_EQ(addrBefore, addrAfter)
      << "overflow growth must not relocate a previously returned entry";
  EXPECT_EQ(map[300].value, 111);
}

// finding 4 behind the SymbolStateMap fix, the two-live-references form from the review: a
// reference obtained before growth must still observe writes made to it
// after growth (i.e. it is the same object, not a stale copy).
TEST_F(SymbolStateMapTest, OverflowReferenceUsableAfterLaterInsertions)
{
  struct TestState
  {
    int value{0};
  };
  SymbolStateMap<TestState, 10> map;

  TestState& a = map[300];
  a.value = 1;
  for (SymbolId sym = 301; sym < 350; ++sym)
  {
    map[sym].value = static_cast<int>(sym);
  }
  a.value = 2;

  EXPECT_EQ(map[300].value, 2);
}

// finding 9 behind the SymbolStateMap fix: clear() reset `initialized` unconditionally but only
// reassigned `flat` when State is move-constructible; for a State holding
// atomics (the documented non-movable case), the old data survived under a
// freshly-cleared flag, so the next operator[] handed back the previous
// run's values looking like a brand new entry.
TEST_F(SymbolStateMapTest, ClearResetsNonMovableState)
{
  struct AtomicState
  {
    std::atomic<int> value{0};
  };
  static_assert(!std::is_move_constructible_v<AtomicState>,
                "this test is only meaningful for a non-movable State");

  SymbolStateMap<AtomicState> map;
  map[3].value.store(77, std::memory_order_relaxed);
  ASSERT_EQ(map[3].value.load(std::memory_order_relaxed), 77);

  map.clear();

  EXPECT_EQ(map[3].value.load(std::memory_order_relaxed), 0)
      << "clear() must reset a non-movable State's data, not just its "
         "initialized flag";
}

// a follow-up to the SymbolStateMap fix: clear() unconditionally resetting `flat`
// and the scratch slot is not enough on its own -- the overflow container
// itself has to be emptied too, or a symbol that lived past kMaxSymbols
// keeps answering with its pre-clear() state even though `initialized` (the
// dense-range flag) was reset. Pins the `_overflowStorage.clear()` call.
TEST_F(SymbolStateMapTest, ClearEmptiesOverflowStorage)
{
  struct TestState
  {
    int value{0};
  };
  SymbolStateMap<TestState, 10> map;

  map[300].value = 111;
  map[301].value = 222;
  map[302].value = 333;
  ASSERT_EQ(map.size(), 3u);

  map.clear();

  EXPECT_EQ(map.size(), 0u)
      << "clear() must empty the overflow container, not just the flat table";

  int visited = 0;
  map.forEach([&visited](SymbolId, const TestState&)
              { ++visited; });
  EXPECT_EQ(visited, 0) << "forEach must not visit a stale overflow entry";

  EXPECT_EQ(map.tryGet(300), nullptr)
      << "tryGet on a former overflow symbol must report missing, not stale";

  // A read after clear() creates a fresh entry -- it must not resurrect the
  // value the old (uncleared) overflow entry held.
  EXPECT_EQ(map[300].value, 0);
}

// a follow-up to the SymbolStateMap fix, the scratch-slot half: for a non-movable
// State, an out-of-range write lands in the single shared `_overflowScratch`
// slot (see operator[] above), not in the overflow container. clear() has to
// reset that slot too, independently of `_overflowStorage.clear()`, or a
// later out-of-range read on a *different* overflow symbol reads the
// previous symbol's leftover data through the shared slot.
//
// Writing an out-of-range symbol for a non-movable State takes one of two
// paths depending on NDEBUG, the same split used in test_cex_coordination.cpp
// for the identical guard: with NDEBUG unset (a debug/sanitizer build) the
// `assert(false)` in operator[] fires before the scratch slot is ever
// touched, so this test would abort rather than pin clear(); it is only
// meaningful in a release-style build (NDEBUG set), which is what the
// project's default and RelWithDebInfo builds use.
#ifdef NDEBUG

TEST_F(SymbolStateMapTest, ClearResetsOverflowScratchForNonMovableState)
{
  struct AtomicState
  {
    std::atomic<int> value{0};
  };
  static_assert(!std::is_move_constructible_v<AtomicState>,
                "this test is only meaningful for a non-movable State");

  SymbolStateMap<AtomicState, 10> map;

  // Out of range for a non-movable State: routed to the shared scratch slot,
  // not to a per-symbol overflow entry.
  map[300].value.store(55, std::memory_order_relaxed);
  ASSERT_EQ(map[300].value.load(std::memory_order_relaxed), 55);

  map.clear();

  EXPECT_EQ(map[301].value.load(std::memory_order_relaxed), 0)
      << "clear() must reset the shared overflow scratch slot, not just the "
         "flat table, or a different out-of-range symbol reads a previous "
         "symbol's leftover value through it";
}

#endif  // NDEBUG

// Const access must not create entries or mark a symbol initialized -- this
// already held before the SymbolStateMap fix (the non-const overload is the one that had
// the bug; see the PositionTracker tests below for where that actually bit).
// Kept here as a direct regression guard on SymbolStateMap's own contract.
TEST_F(SymbolStateMapTest, ConstAccessDoesNotMarkInitialized)
{
  struct TestState
  {
    int value{0};
  };
  SymbolStateMap<TestState, 10> map;
  const auto& constMap = map;

  EXPECT_EQ(constMap[5].value, 0);
  EXPECT_EQ(constMap[500].value, 0);  // overflow range
  EXPECT_EQ(map.size(), 0u);
  EXPECT_FALSE(map.contains(5));
  EXPECT_FALSE(map.contains(500));
}

class SymbolContextTest : public ::testing::Test
{
 protected:
  std::pmr::monotonic_buffer_resource _resource{4096};
};

TEST_F(SymbolContextTest, MidPriceCalculation)
{
  SymbolContext ctx(Price::fromDouble(0.01));

  BookUpdateEvent ev(&_resource);
  ev.update.type = BookUpdateType::SNAPSHOT;
  ev.update.bids.emplace_back(Price::fromDouble(100.0), Quantity::fromDouble(1.0));
  ev.update.asks.emplace_back(Price::fromDouble(101.0), Quantity::fromDouble(1.0));

  ctx.book.applyBookUpdate(ev);

  auto mid = ctx.mid();
  ASSERT_TRUE(mid.has_value());
  EXPECT_NEAR(mid->toDouble(), 100.5, 0.01);
}

TEST_F(SymbolContextTest, UnrealizedPnlLong)
{
  SymbolContext ctx(Price::fromDouble(0.01));
  ctx.position = Quantity::fromDouble(10.0);
  ctx.avgEntryPrice = Price::fromDouble(100.0);

  auto pnl = ctx.unrealizedPnl(Price::fromDouble(110.0));
  ASSERT_TRUE(pnl.has_value());
  EXPECT_NEAR(*pnl, 100.0, 0.01);
}

TEST_F(SymbolContextTest, UnrealizedPnlShort)
{
  SymbolContext ctx(Price::fromDouble(0.01));
  ctx.position = Quantity::fromDouble(-10.0);
  ctx.avgEntryPrice = Price::fromDouble(100.0);

  auto pnl = ctx.unrealizedPnl(Price::fromDouble(90.0));
  ASSERT_TRUE(pnl.has_value());
  EXPECT_NEAR(*pnl, 100.0, 0.01);
}

TEST_F(SymbolContextTest, PositionFlags)
{
  SymbolContext ctx(Price::fromDouble(0.01));

  EXPECT_TRUE(ctx.isFlat());
  EXPECT_FALSE(ctx.isLong());
  EXPECT_FALSE(ctx.isShort());

  ctx.position = Quantity::fromDouble(1.0);
  EXPECT_TRUE(ctx.isLong());
  EXPECT_FALSE(ctx.isFlat());

  ctx.position = Quantity::fromDouble(-1.0);
  EXPECT_TRUE(ctx.isShort());
}

class TestStrategy : public Strategy
{
 public:
  TestStrategy(std::vector<SymbolId> syms, const SymbolRegistry& registry)
      : Strategy(1, std::move(syms), registry)
  {
  }

  TestStrategy(SubscriberId id, std::vector<SymbolId> syms, const SymbolRegistry& registry)
      : Strategy(id, std::move(syms), registry)
  {
  }

  void start() override {}
  void stop() override {}

  int tradeCount{0};
  int bookCount{0};

  using Strategy::ctx;

 protected:
  void onSymbolTrade(SymbolContext& c, const TradeEvent& ev) override { ++tradeCount; }

  void onSymbolBook(SymbolContext& c, const BookUpdateEvent& ev) override { ++bookCount; }
};

class MultiSymbolStrategyTest : public ::testing::Test
{
 protected:
  std::pmr::monotonic_buffer_resource _resource{4096};
};

TEST_F(MultiSymbolStrategyTest, FiltersUnsubscribedSymbols)
{
  SymbolRegistry registry;
  populateRegistry(registry, {1, 2});
  TestStrategy strategy({1, 2}, registry);

  TradeEvent ev1, ev2, ev3;
  ev1.trade.symbol = 1;
  ev2.trade.symbol = 2;
  ev3.trade.symbol = 3;

  strategy.onTrade(ev1);
  strategy.onTrade(ev2);
  strategy.onTrade(ev3);

  EXPECT_EQ(strategy.tradeCount, 2);
}

TEST_F(MultiSymbolStrategyTest, ContextAccessible)
{
  SymbolRegistry registry;
  populateRegistry(registry, {1, 2});
  TestStrategy strategy({1, 2}, registry);

  TradeEvent ev;
  ev.trade.symbol = 1;
  ev.trade.price = Price::fromDouble(100.0);

  strategy.onTrade(ev);

  EXPECT_EQ(strategy.ctx(1).lastTradePrice.toDouble(), 100.0);
}

TEST_F(MultiSymbolStrategyTest, SpreadCalculation)
{
  SymbolRegistry registry;
  populateRegistry(registry, {1, 2});
  TestStrategy strategy({1, 2}, registry);

  BookUpdateEvent ev1(&_resource);
  ev1.update.symbol = 1;
  ev1.update.type = BookUpdateType::SNAPSHOT;
  ev1.update.bids.emplace_back(Price::fromDouble(100.0), Quantity::fromDouble(1.0));
  ev1.update.asks.emplace_back(Price::fromDouble(101.0), Quantity::fromDouble(1.0));

  BookUpdateEvent ev2(&_resource);
  ev2.update.symbol = 2;
  ev2.update.type = BookUpdateType::SNAPSHOT;
  ev2.update.bids.emplace_back(Price::fromDouble(50.0), Quantity::fromDouble(1.0));
  ev2.update.asks.emplace_back(Price::fromDouble(51.0), Quantity::fromDouble(1.0));

  strategy.onBookUpdate(ev1);
  strategy.onBookUpdate(ev2);

  auto spreadOpt = spread(strategy.ctx(1), strategy.ctx(2));
  ASSERT_TRUE(spreadOpt.has_value());
  EXPECT_NEAR(spreadOpt->toDouble(), 50.0, 0.5);
}

TEST_F(MultiSymbolStrategyTest, TopOfBookFramesDoNotReachTheStrategy)
{
  SymbolRegistry registry;
  populateRegistry(registry, {1});
  TestStrategy strategy({1}, registry);

  BookUpdateEvent snap(&_resource);
  snap.update.symbol = 1;
  snap.update.type = BookUpdateType::SNAPSHOT;
  snap.update.exchangeTsNs = UnixNanos::fromRaw(1'000);
  snap.update.bids.emplace_back(Price::fromDouble(100.0), Quantity::fromDouble(1.0));
  snap.update.asks.emplace_back(Price::fromDouble(101.0), Quantity::fromDouble(1.0));
  strategy.onBookUpdate(snap);
  ASSERT_EQ(strategy.bookCount, 1);

  // The context's depth book ignores TOP_OF_BOOK, so the strategy must not
  // be called either: onSymbolBook would see the previous snapshot's top,
  // and lastUpdateNs would claim that stale top is as fresh as the touch.
  BookUpdateEvent top(&_resource);
  top.update.symbol = 1;
  top.update.type = BookUpdateType::TOP_OF_BOOK;
  top.update.exchangeTsNs = UnixNanos::fromRaw(2'000);
  top.update.bids.emplace_back(Price::fromDouble(100.5), Quantity::fromDouble(2.0));
  top.update.asks.emplace_back(Price::fromDouble(100.6), Quantity::fromDouble(2.0));
  strategy.onBookUpdate(top);

  EXPECT_EQ(strategy.bookCount, 1);
  EXPECT_EQ(strategy.ctx(1).lastUpdateNs, 1'000);
  EXPECT_EQ(strategy.ctx(1).book.bestBid(), Price::fromDouble(100.0));
}

class PositionTrackerTest : public ::testing::Test
{
};

TEST_F(PositionTrackerTest, TracksBuyPosition)
{
  PositionTracker tracker(1);

  Order order{.id = 1,
              .side = Side::BUY,
              .price = Price::fromDouble(100.0),
              .quantity = Quantity::fromDouble(10.0),
              .type = OrderType::MARKET,
              .symbol = 1};

  tracker.onOrderFilled(order);

  EXPECT_EQ(tracker.getPosition(1).toDouble(), 10.0);
  EXPECT_EQ(tracker.getAvgEntryPrice(1).toDouble(), 100.0);
}

TEST_F(PositionTrackerTest, TracksSellPosition)
{
  PositionTracker tracker(1);

  Order order{.id = 1,
              .side = Side::SELL,
              .price = Price::fromDouble(100.0),
              .quantity = Quantity::fromDouble(10.0),
              .type = OrderType::MARKET,
              .symbol = 1};

  tracker.onOrderFilled(order);

  EXPECT_EQ(tracker.getPosition(1).toDouble(), -10.0);
  EXPECT_EQ(tracker.getAvgEntryPrice(1).toDouble(), 100.0);
}

TEST_F(PositionTrackerTest, CalculatesRealizedPnlOnClose)
{
  PositionTracker tracker(1);

  Order buy{.id = 1,
            .side = Side::BUY,
            .price = Price::fromDouble(100.0),
            .quantity = Quantity::fromDouble(10.0),
            .type = OrderType::MARKET,
            .symbol = 1};
  tracker.onOrderFilled(buy);

  Order sell{.id = 2,
             .side = Side::SELL,
             .price = Price::fromDouble(110.0),
             .quantity = Quantity::fromDouble(10.0),
             .type = OrderType::MARKET,
             .symbol = 1};
  tracker.onOrderFilled(sell);

  EXPECT_EQ(tracker.getPosition(1).toDouble(), 0.0);
  EXPECT_NEAR(tracker.getRealizedPnl(1).toDouble(), 100.0, 0.01);
}

TEST_F(PositionTrackerTest, VWAPOnAddingToPosition)
{
  PositionTracker tracker(1);

  Order buy1{.id = 1,
             .side = Side::BUY,
             .price = Price::fromDouble(100.0),
             .quantity = Quantity::fromDouble(10.0),
             .type = OrderType::MARKET,
             .symbol = 1};
  tracker.onOrderFilled(buy1);

  Order buy2{.id = 2,
             .side = Side::BUY,
             .price = Price::fromDouble(110.0),
             .quantity = Quantity::fromDouble(10.0),
             .type = OrderType::MARKET,
             .symbol = 1};
  tracker.onOrderFilled(buy2);

  EXPECT_EQ(tracker.getPosition(1).toDouble(), 20.0);
  EXPECT_EQ(tracker.getAvgEntryPrice(1).toDouble(), 105.0);
}

TEST_F(PositionTrackerTest, PartialFillUpdatesPosition)
{
  PositionTracker tracker(1);

  Order order{.id = 1,
              .side = Side::BUY,
              .price = Price::fromDouble(100.0),
              .quantity = Quantity::fromDouble(10.0),
              .type = OrderType::LIMIT,
              .symbol = 1};

  tracker.onOrderPartiallyFilled(order, Quantity::fromDouble(5.0));

  EXPECT_EQ(tracker.getPosition(1).toDouble(), 5.0);
}

TEST_F(PositionTrackerTest, MultipleSymbolsTrackedSeparately)
{
  PositionTracker tracker(1);

  Order buy1{.id = 1,
             .side = Side::BUY,
             .price = Price::fromDouble(100.0),
             .quantity = Quantity::fromDouble(10.0),
             .type = OrderType::MARKET,
             .symbol = 1};
  tracker.onOrderFilled(buy1);

  Order buy2{.id = 2,
             .side = Side::BUY,
             .price = Price::fromDouble(50.0),
             .quantity = Quantity::fromDouble(20.0),
             .type = OrderType::MARKET,
             .symbol = 2};
  tracker.onOrderFilled(buy2);

  EXPECT_EQ(tracker.getPosition(1).toDouble(), 10.0);
  EXPECT_EQ(tracker.getPosition(2).toDouble(), 20.0);
  EXPECT_EQ(tracker.getAvgEntryPrice(1).toDouble(), 100.0);
  EXPECT_EQ(tracker.getAvgEntryPrice(2).toDouble(), 50.0);
}

TEST_F(PositionTrackerTest, FIFOPnlCalculation)
{
  PositionTracker tracker(1, CostBasisMethod::FIFO);

  Order buy1{.id = 1,
             .side = Side::BUY,
             .price = Price::fromDouble(100.0),
             .quantity = Quantity::fromDouble(10.0),
             .type = OrderType::MARKET,
             .symbol = 1};
  tracker.onOrderFilled(buy1);

  Order buy2{.id = 2,
             .side = Side::BUY,
             .price = Price::fromDouble(120.0),
             .quantity = Quantity::fromDouble(10.0),
             .type = OrderType::MARKET,
             .symbol = 1};
  tracker.onOrderFilled(buy2);

  Order sell{.id = 3,
             .side = Side::SELL,
             .price = Price::fromDouble(115.0),
             .quantity = Quantity::fromDouble(10.0),
             .type = OrderType::MARKET,
             .symbol = 1};
  tracker.onOrderFilled(sell);

  EXPECT_EQ(tracker.getPosition(1).toDouble(), 10.0);
  EXPECT_NEAR(tracker.getRealizedPnl(1).toDouble(), 150.0, 0.01);
  EXPECT_EQ(tracker.getAvgEntryPrice(1).toDouble(), 120.0);
}

TEST_F(PositionTrackerTest, LIFOPnlCalculation)
{
  PositionTracker tracker(1, CostBasisMethod::LIFO);

  Order buy1{.id = 1,
             .side = Side::BUY,
             .price = Price::fromDouble(100.0),
             .quantity = Quantity::fromDouble(10.0),
             .type = OrderType::MARKET,
             .symbol = 1};
  tracker.onOrderFilled(buy1);

  Order buy2{.id = 2,
             .side = Side::BUY,
             .price = Price::fromDouble(120.0),
             .quantity = Quantity::fromDouble(10.0),
             .type = OrderType::MARKET,
             .symbol = 1};
  tracker.onOrderFilled(buy2);

  Order sell{.id = 3,
             .side = Side::SELL,
             .price = Price::fromDouble(115.0),
             .quantity = Quantity::fromDouble(10.0),
             .type = OrderType::MARKET,
             .symbol = 1};
  tracker.onOrderFilled(sell);

  EXPECT_EQ(tracker.getPosition(1).toDouble(), 10.0);
  EXPECT_NEAR(tracker.getRealizedPnl(1).toDouble(), -50.0, 0.01);
  EXPECT_EQ(tracker.getAvgEntryPrice(1).toDouble(), 100.0);
}

TEST_F(PositionTrackerTest, AveragePnlCalculation)
{
  PositionTracker tracker(1, CostBasisMethod::AVERAGE);

  Order buy1{.id = 1,
             .side = Side::BUY,
             .price = Price::fromDouble(100.0),
             .quantity = Quantity::fromDouble(10.0),
             .type = OrderType::MARKET,
             .symbol = 1};
  tracker.onOrderFilled(buy1);

  Order buy2{.id = 2,
             .side = Side::BUY,
             .price = Price::fromDouble(120.0),
             .quantity = Quantity::fromDouble(10.0),
             .type = OrderType::MARKET,
             .symbol = 1};
  tracker.onOrderFilled(buy2);

  Order sell{.id = 3,
             .side = Side::SELL,
             .price = Price::fromDouble(115.0),
             .quantity = Quantity::fromDouble(10.0),
             .type = OrderType::MARKET,
             .symbol = 1};
  tracker.onOrderFilled(sell);

  EXPECT_EQ(tracker.getPosition(1).toDouble(), 10.0);
  EXPECT_NEAR(tracker.getRealizedPnl(1).toDouble(), 50.0, 0.01);
  EXPECT_EQ(tracker.getAvgEntryPrice(1).toDouble(), 110.0);
}

TEST_F(PositionTrackerTest, ShortPositionFIFO)
{
  PositionTracker tracker(1, CostBasisMethod::FIFO);

  Order sell1{.id = 1,
              .side = Side::SELL,
              .price = Price::fromDouble(120.0),
              .quantity = Quantity::fromDouble(10.0),
              .type = OrderType::MARKET,
              .symbol = 1};
  tracker.onOrderFilled(sell1);

  Order sell2{.id = 2,
              .side = Side::SELL,
              .price = Price::fromDouble(100.0),
              .quantity = Quantity::fromDouble(10.0),
              .type = OrderType::MARKET,
              .symbol = 1};
  tracker.onOrderFilled(sell2);

  Order buy{.id = 3,
            .side = Side::BUY,
            .price = Price::fromDouble(105.0),
            .quantity = Quantity::fromDouble(10.0),
            .type = OrderType::MARKET,
            .symbol = 1};
  tracker.onOrderFilled(buy);

  EXPECT_EQ(tracker.getPosition(1).toDouble(), -10.0);
  EXPECT_NEAR(tracker.getRealizedPnl(1).toDouble(), 150.0, 0.01);
  EXPECT_EQ(tracker.getAvgEntryPrice(1).toDouble(), 100.0);
}

TEST_F(PositionTrackerTest, PartialLotClose)
{
  PositionTracker tracker(1, CostBasisMethod::FIFO);

  Order buy{.id = 1,
            .side = Side::BUY,
            .price = Price::fromDouble(100.0),
            .quantity = Quantity::fromDouble(20.0),
            .type = OrderType::MARKET,
            .symbol = 1};
  tracker.onOrderFilled(buy);

  Order sell{.id = 2,
             .side = Side::SELL,
             .price = Price::fromDouble(110.0),
             .quantity = Quantity::fromDouble(5.0),
             .type = OrderType::MARKET,
             .symbol = 1};
  tracker.onOrderFilled(sell);

  EXPECT_EQ(tracker.getPosition(1).toDouble(), 15.0);
  EXPECT_NEAR(tracker.getRealizedPnl(1).toDouble(), 50.0, 0.01);
  EXPECT_EQ(tracker.getAvgEntryPrice(1).toDouble(), 100.0);
}

TEST_F(PositionTrackerTest, FlipPositionLongToShort)
{
  PositionTracker tracker(1, CostBasisMethod::FIFO);

  Order buy{.id = 1,
            .side = Side::BUY,
            .price = Price::fromDouble(100.0),
            .quantity = Quantity::fromDouble(10.0),
            .type = OrderType::MARKET,
            .symbol = 1};
  tracker.onOrderFilled(buy);

  Order sell{.id = 2,
             .side = Side::SELL,
             .price = Price::fromDouble(110.0),
             .quantity = Quantity::fromDouble(15.0),
             .type = OrderType::MARKET,
             .symbol = 1};
  tracker.onOrderFilled(sell);

  EXPECT_EQ(tracker.getPosition(1).toDouble(), -5.0);
  EXPECT_NEAR(tracker.getRealizedPnl(1).toDouble(), 100.0, 0.01);
  EXPECT_EQ(tracker.getAvgEntryPrice(1).toDouble(), 110.0);
}

// finding 5 behind the SymbolStateMap fix: getPosition/getAvgEntryPrice/getRealizedPnl are const,
// but reached SymbolStateMap through a `mutable` member -- which picks
// SymbolStateMap's non-const operator[] regardless of the caller being
// const, and that overload marks the symbol initialized on a plain read (and
// for a symbol past the flat table, allocates an overflow entry). A symbol
// that never traded then showed up in size()/forEach()/getTotalRealizedPnl()
// merely because something asked about it. trackedSymbolCount() (added
// alongside this fix) exposes the count for the test to check directly.
TEST_F(PositionTrackerTest, ConstQueriesDoNotMarkSymbolInitialized)
{
  const PositionTracker tracker(1);

  ASSERT_EQ(tracker.trackedSymbolCount(), 0u);

  EXPECT_EQ(tracker.getPosition(7).toDouble(), 0.0);
  EXPECT_EQ(tracker.getAvgEntryPrice(7).toDouble(), 0.0);
  EXPECT_EQ(tracker.getRealizedPnl(7).toDouble(), 0.0);
  EXPECT_FALSE(tracker.getAverageEntryPrice(7).has_value());

  EXPECT_EQ(tracker.trackedSymbolCount(), 0u)
      << "a read-only query on a symbol that never traded must not make it "
         "appear tracked";
  EXPECT_EQ(tracker.getTotalRealizedPnl().toDouble(), 0.0);
}

// Same finding, the overflow-range half: querying a symbol past the flat
// table's 256 slots used to allocate an overflow entry from a const method.
TEST_F(PositionTrackerTest, ConstQueriesDoNotAllocateOverflowEntry)
{
  const PositionTracker tracker(1);
  constexpr SymbolId kOverflowSymbol = 300;

  EXPECT_EQ(tracker.getPosition(kOverflowSymbol).toDouble(), 0.0);
  EXPECT_EQ(tracker.getAvgEntryPrice(kOverflowSymbol).toDouble(), 0.0);
  EXPECT_EQ(tracker.getRealizedPnl(kOverflowSymbol).toDouble(), 0.0);

  EXPECT_EQ(tracker.trackedSymbolCount(), 0u);
}

// A traded symbol must of course still count -- the fix must not make
// PositionTracker forget real state, only stop inventing it on reads.
TEST_F(PositionTrackerTest, TradedSymbolIsTracked)
{
  PositionTracker tracker(1);

  Order buy{.id = 1,
            .side = Side::BUY,
            .price = Price::fromDouble(100.0),
            .quantity = Quantity::fromDouble(1.0),
            .type = OrderType::MARKET,
            .symbol = 42};
  tracker.onOrderFilled(buy);

  EXPECT_EQ(tracker.trackedSymbolCount(), 1u);
}

// Tests for Strategy integration with OrderTracker and PositionManager

class SignalCapture : public ISignalHandler
{
 public:
  void onSignal(const Signal& signal) override { signals.push_back(signal); }
  std::vector<Signal> signals;
};

class StrategyIntegrationTest : public ::testing::Test
{
 protected:
  SymbolRegistry registry;
  SignalCapture signalCapture;
  OrderTracker orderTracker;
  PositionTracker positionTracker{1};

  void SetUp() override { populateRegistry(registry, {1, 2}); }
};

TEST_F(StrategyIntegrationTest, EmitMarketBuyReturnsOrderId)
{
  class TestableStrategy : public TestStrategy
  {
   public:
    using Strategy::emitMarketBuy;
    using TestStrategy::TestStrategy;
  };

  TestableStrategy testable({1}, registry);
  testable.setSignalHandler(&signalCapture);

  OrderId id1 = testable.emitMarketBuy(1, Quantity::fromDouble(10.0));
  OrderId id2 = testable.emitMarketBuy(1, Quantity::fromDouble(5.0));

  EXPECT_NE(id1, id2);
  EXPECT_EQ(signalCapture.signals.size(), 2);
  EXPECT_EQ(signalCapture.signals[0].orderId, id1);
  EXPECT_EQ(signalCapture.signals[1].orderId, id2);
}

TEST_F(StrategyIntegrationTest, EmitModifyGeneratesModifySignal)
{
  class TestableStrategy : public TestStrategy
  {
   public:
    using Strategy::emitLimitBuy;
    using Strategy::emitModify;
    using TestStrategy::TestStrategy;
  };

  TestableStrategy strategy({1}, registry);
  strategy.setSignalHandler(&signalCapture);

  OrderId id = strategy.emitLimitBuy(1, Price::fromDouble(100.0), Quantity::fromDouble(10.0));
  strategy.emitModify(id, Price::fromDouble(99.0), Quantity::fromDouble(5.0));

  EXPECT_EQ(signalCapture.signals.size(), 2);
  EXPECT_EQ(signalCapture.signals[1].type, SignalType::Modify);
  EXPECT_EQ(signalCapture.signals[1].orderId, id);
  EXPECT_EQ(signalCapture.signals[1].newPrice.toDouble(), 99.0);
  EXPECT_EQ(signalCapture.signals[1].newQuantity.toDouble(), 5.0);
}

TEST_F(StrategyIntegrationTest, GetOrderStatusFromTracker)
{
  class TestableStrategy : public TestStrategy
  {
   public:
    using Strategy::getOrder;
    using Strategy::getOrderStatus;
    using TestStrategy::TestStrategy;
  };

  TestableStrategy strategy({1}, registry);
  strategy.setOrderTracker(&orderTracker);

  Order order{.id = 42, .side = Side::BUY, .price = Price::fromDouble(100.0), .quantity = Quantity::fromDouble(10.0)};
  orderTracker.onSubmitted(order, "exch-123");

  auto status = strategy.getOrderStatus(42);
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(*status, OrderEventStatus::SUBMITTED);

  auto orderState = strategy.getOrder(42);
  ASSERT_TRUE(orderState.has_value());
  EXPECT_EQ(orderState->exchangeOrderId, "exch-123");
}

TEST_F(StrategyIntegrationTest, GetOrderStatusWithoutTrackerReturnsNullopt)
{
  class TestableStrategy : public TestStrategy
  {
   public:
    using Strategy::getOrderStatus;
    using TestStrategy::TestStrategy;
  };

  TestableStrategy strategy({1}, registry);
  // No tracker set

  auto status = strategy.getOrderStatus(42);
  EXPECT_FALSE(status.has_value());
}

TEST_F(StrategyIntegrationTest, PositionFromPositionManager)
{
  class TestableStrategy : public TestStrategy
  {
   public:
    using Strategy::position;
    using TestStrategy::TestStrategy;
  };

  TestableStrategy strategy({1, 2}, registry);
  strategy.setPositionManager(&positionTracker);

  Order buy{.id = 1,
            .side = Side::BUY,
            .price = Price::fromDouble(100.0),
            .quantity = Quantity::fromDouble(10.0),
            .type = OrderType::MARKET,
            .symbol = 1};
  positionTracker.onOrderFilled(buy);

  EXPECT_EQ(strategy.position(1).toDouble(), 10.0);
  EXPECT_EQ(strategy.position(2).toDouble(), 0.0);
  EXPECT_EQ(strategy.position().toDouble(), 10.0);  // First symbol
}

TEST_F(StrategyIntegrationTest, PositionWithoutManagerReturnsZero)
{
  class TestableStrategy : public TestStrategy
  {
   public:
    using Strategy::position;
    using TestStrategy::TestStrategy;
  };

  TestableStrategy strategy({1}, registry);
  // No position manager set

  EXPECT_EQ(strategy.position(1).toDouble(), 0.0);
}

// Order ids are namespaced by subscriber id, so two strategies sharing a bus
// never collide. Strategies that share a subscriber id share an id space --
// but a subscriber id is a subscriber's identity, and two of them holding the
// same one is already broken for every bus they sit on.
TEST_F(StrategyIntegrationTest, OrderIdsAreUniqueAcrossStrategies)
{
  class TestableStrategy : public TestStrategy
  {
   public:
    using Strategy::emitMarketBuy;
    using TestStrategy::TestStrategy;
  };

  // On the heap deliberately. A Strategy carries its per-symbol contexts by
  // value and each context holds a full 512-level book: roughly 2 MB per
  // object in a release build and 4 MB in a checked one, so two in a single
  // stack frame overrun the default 8 MB stack wherever scale checks are on.
  auto strategy1 = std::make_unique<TestableStrategy>(SubscriberId{1},
                                                      std::vector<SymbolId>{1}, registry);
  auto strategy2 = std::make_unique<TestableStrategy>(SubscriberId{2},
                                                      std::vector<SymbolId>{2}, registry);
  strategy1->setSignalHandler(&signalCapture);
  strategy2->setSignalHandler(&signalCapture);

  OrderId id1 = strategy1->emitMarketBuy(1, Quantity::fromDouble(1.0));
  OrderId id2 = strategy2->emitMarketBuy(2, Quantity::fromDouble(1.0));
  OrderId id3 = strategy1->emitMarketBuy(1, Quantity::fromDouble(1.0));

  EXPECT_NE(id1, id2);
  EXPECT_NE(id2, id3);
  EXPECT_NE(id1, id3);
}

// A second run of the same strategy in the same process must hand out the
// same order ids as the first. A process-wide counter made a grid search, a
// walk-forward pass or any batch runner produce traces that could not be
// diffed byte for byte against each other.
TEST_F(StrategyIntegrationTest, OrderIdsRestartForEachStrategyInstance)
{
  class TestableStrategy : public TestStrategy
  {
   public:
    using Strategy::emitMarketBuy;
    using TestStrategy::TestStrategy;
  };

  std::vector<OrderId> firstRun;
  {
    auto strategy = std::make_unique<TestableStrategy>(SubscriberId{7},
                                                       std::vector<SymbolId>{1}, registry);
    strategy->setSignalHandler(&signalCapture);
    for (int i = 0; i < 3; ++i)
    {
      firstRun.push_back(strategy->emitMarketBuy(1, Quantity::fromDouble(1.0)));
    }
  }

  std::vector<OrderId> secondRun;
  {
    auto strategy = std::make_unique<TestableStrategy>(SubscriberId{7},
                                                       std::vector<SymbolId>{1}, registry);
    strategy->setSignalHandler(&signalCapture);
    for (int i = 0; i < 3; ++i)
    {
      secondRun.push_back(strategy->emitMarketBuy(1, Quantity::fromDouble(1.0)));
    }
  }

  EXPECT_EQ(firstRun, secondRun);
}

// ============================================================
// Ownership of the per-symbol state
//
// A strategy sits on three buses and each runs its own consumer thread, so
// a trade and a book update for the same symbol reach the strategy at the
// same instant on two different threads. Everything below drives that
// shape directly, without a bus, so the tests run everywhere the suite
// does -- ThreadSanitizer included, where this shape used to report races
// in SymbolStateMap::operator[], NLevelOrderBook::applyBookUpdate,
// Strategy::onBookUpdate and Decimal::raw().
// ============================================================

namespace
{

class OwnershipStrategy : public Strategy
{
 public:
  OwnershipStrategy(std::vector<SymbolId> syms, const SymbolRegistry& registry)
      : Strategy(1, std::move(syms), registry)
  {
  }

  void start() override {}
  void stop() override {}

  std::atomic<int> trades{0};
  std::atomic<int> books{0};
  std::atomic<int> tornReads{0};

  using Strategy::ctx;

 protected:
  // Both hooks read the whole context while they hold it. Every book
  // update this test publishes carries the same two levels, so any
  // best bid / best ask other than 100 / 101 means the hook was handed a
  // context another thread was rewriting underneath it.
  void onSymbolTrade(SymbolContext& c, const TradeEvent&) override
  {
    checkBook(c);
    trades.fetch_add(1, std::memory_order_relaxed);
  }

  void onSymbolBook(SymbolContext& c, const BookUpdateEvent&) override
  {
    checkBook(c);
    books.fetch_add(1, std::memory_order_relaxed);
  }

 private:
  void checkBook(const SymbolContext& c)
  {
    auto bid = c.book.bestBid();
    auto ask = c.book.bestAsk();
    if (!bid || !ask)
    {
      return;  // before the first snapshot lands
    }
    if (std::abs(bid->toDouble() - 100.0) > 1e-6 ||
        std::abs(ask->toDouble() - 101.0) > 1e-6)
    {
      tornReads.fetch_add(1, std::memory_order_relaxed);
    }
  }
};

}  // namespace

TEST(StrategyStateOwnership, ConcurrentTradeAndBookForOneSymbolStayCoherent)
{
  constexpr int kEvents = 20000;
  constexpr SymbolId kSym = 1;

  SymbolRegistry registry;
  populateRegistry(registry, {kSym});
  auto strategy = std::make_unique<OwnershipStrategy>(std::vector<SymbolId>{kSym}, registry);

  std::atomic<bool> go{false};

  std::thread tradeThread(
      [&]
      {
        while (!go.load(std::memory_order_acquire))
        {
        }
        for (int i = 0; i < kEvents; ++i)
        {
          TradeEvent ev;
          ev.trade.symbol = kSym;
          ev.trade.price = Price::fromDouble(100.5);
          ev.trade.quantity = Quantity::fromDouble(1.0);
          ev.trade.exchangeTsNs = UnixNanos(1000000000LL + i);
          strategy->onTrade(ev);
        }
      });

  std::thread bookThread(
      [&]
      {
        std::pmr::monotonic_buffer_resource arena;
        while (!go.load(std::memory_order_acquire))
        {
        }
        for (int i = 0; i < kEvents; ++i)
        {
          BookUpdateEvent ev(&arena);
          ev.update.symbol = kSym;
          ev.update.type = BookUpdateType::SNAPSHOT;
          ev.update.exchangeTsNs = UnixNanos(1000000000LL + i);
          ev.update.bids.emplace_back(Price::fromDouble(100.0), Quantity::fromDouble(1.0));
          ev.update.asks.emplace_back(Price::fromDouble(101.0), Quantity::fromDouble(1.0));
          strategy->onBookUpdate(ev);
        }
      });

  go.store(true, std::memory_order_release);
  tradeThread.join();
  bookThread.join();

  EXPECT_EQ(strategy->trades.load(), kEvents);
  EXPECT_EQ(strategy->books.load(), kEvents);
  EXPECT_EQ(strategy->tornReads.load(), 0)
      << "a hook was handed a context another thread was rewriting";
  EXPECT_NEAR(strategy->ctx(kSym).mid()->toDouble(), 100.5, 1e-6);
}

// Two symbols in parallel is the reason the lock is per symbol rather than
// per strategy. Nothing here can deadlock, but a coarser lock would turn
// this into a serial run, and a broken per-symbol mapping would cross the
// two symbols' state.
TEST(StrategyStateOwnership, DifferentSymbolsDispatchInParallel)
{
  constexpr int kEvents = 20000;

  SymbolRegistry registry;
  populateRegistry(registry, {1, 2});
  auto strategy = std::make_unique<OwnershipStrategy>(std::vector<SymbolId>{1, 2}, registry);

  auto pump = [&](SymbolId sym, double price)
  {
    for (int i = 0; i < kEvents; ++i)
    {
      TradeEvent ev;
      ev.trade.symbol = sym;
      ev.trade.price = Price::fromDouble(price);
      ev.trade.quantity = Quantity::fromDouble(1.0);
      ev.trade.exchangeTsNs = UnixNanos(1000000000LL + i);
      strategy->onTrade(ev);
    }
  };

  std::thread a([&]
                { pump(1, 100.0); });
  std::thread b([&]
                { pump(2, 200.0); });
  a.join();
  b.join();

  EXPECT_EQ(strategy->trades.load(), 2 * kEvents);
  EXPECT_EQ(strategy->ctx(1).lastTradePrice.toDouble(), 100.0);
  EXPECT_EQ(strategy->ctx(2).lastTradePrice.toDouble(), 200.0);
}

namespace
{

// A hook that emits an order the executor fills straight away, which
// re-enters the strategy for the same symbol on the same thread. The
// simulated executor does exactly this in a backtest. A plain per-symbol
// mutex would deadlock here; the test is a liveness test, and a regression
// hangs it rather than failing an assertion.
class ReentrantStrategy : public Strategy
{
 public:
  ReentrantStrategy(std::vector<SymbolId> syms, const SymbolRegistry& registry)
      : Strategy(1, std::move(syms), registry)
  {
  }

  void start() override {}
  void stop() override {}

  int fills{0};
  int trades{0};

 protected:
  void onSymbolTrade(SymbolContext&, const TradeEvent& ev) override
  {
    ++trades;
    OrderEvent fill;
    fill.status = OrderEventStatus::FILLED;
    fill.order.id = 1;
    fill.order.symbol = ev.trade.symbol;
    fill.fillQty = Quantity::fromDouble(1.0);
    fill.fillPrice = ev.trade.price;
    onOrderEvent(fill);
  }

  void onSymbolFill(SymbolContext&, const OrderEvent&) override { ++fills; }
};

}  // namespace

TEST(StrategyStateOwnership, SynchronousReentryForTheSameSymbolDoesNotDeadlock)
{
  SymbolRegistry registry;
  populateRegistry(registry, {1});
  auto strategy = std::make_unique<ReentrantStrategy>(std::vector<SymbolId>{1}, registry);

  TradeEvent ev;
  ev.trade.symbol = 1;
  ev.trade.price = Price::fromDouble(100.0);
  ev.trade.quantity = Quantity::fromDouble(1.0);
  strategy->onTrade(ev);

  EXPECT_EQ(strategy->trades, 1);
  EXPECT_EQ(strategy->fills, 1);
}

// The per-symbol context table used to sit inside the strategy object by
// value: 256 slots, each carrying a 512-level book, put the object at
// roughly 2 MB in a release build and 4 MB with LRVX_SCALE_CHECKS on. Two
// of them in one frame overran a default 8 MB stack, and the overrun
// landed in the constructor prologue.
TEST(StrategyStateOwnership, StrategyObjectFitsOnTheStack)
{
  EXPECT_LT(sizeof(Strategy), size_t{64} * 1024)
      << "sizeof(Strategy) = " << sizeof(Strategy);
}

TEST(StrategyStateOwnership, TwoStrategiesInOneStackFrame)
{
  SymbolRegistry registry;
  populateRegistry(registry, {1, 2});

  TestStrategy first(SubscriberId{1}, std::vector<SymbolId>{1}, registry);
  TestStrategy second(SubscriberId{2}, std::vector<SymbolId>{2}, registry);

  TradeEvent ev;
  ev.trade.symbol = 1;
  ev.trade.price = Price::fromDouble(100.0);
  first.onTrade(ev);

  ev.trade.symbol = 2;
  ev.trade.price = Price::fromDouble(200.0);
  second.onTrade(ev);

  EXPECT_EQ(first.tradeCount, 1);
  EXPECT_EQ(second.tradeCount, 1);
  EXPECT_EQ(first.ctx(1).lastTradePrice.toDouble(), 100.0);
  EXPECT_EQ(second.ctx(2).lastTradePrice.toDouble(), 200.0);
}

// The bar ring is per-symbol strategy state too, and it is an
// unordered_map: inserting a new (symbol, timeframe) key rehashes it and
// moves every other symbol's buckets, so a reader on another thread walks
// buckets that are being relocated underneath it.
TEST(StrategyStateOwnership, BarRingSurvivesConcurrentReadsAndWrites)
{
  constexpr int kBars = 5000;

  SymbolRegistry registry;
  populateRegistry(registry, {1, 2, 3, 4});
  auto strategy = std::make_unique<OwnershipStrategy>(
      std::vector<SymbolId>{1, 2, 3, 4}, registry);

  std::atomic<bool> done{false};

  std::thread writer(
      [&]
      {
        for (int i = 0; i < kBars; ++i)
        {
          BarEvent ev;
          ev.symbol = static_cast<SymbolId>(1 + (i % 4));
          ev.barType = BarType::Time;
          ev.barTypeParam = static_cast<uint64_t>(60 + (i % 8));
          ev.bar.close = Price::fromDouble(100.0 + i);
          ev.bar.endTime = TimePoint{std::chrono::nanoseconds{1000000000LL + i}};
          strategy->onBar(ev);
        }
        done.store(true, std::memory_order_release);
      });

  std::thread reader(
      [&]
      {
        while (!done.load(std::memory_order_acquire))
        {
          for (SymbolId sym = 1; sym <= 4; ++sym)
          {
            (void)strategy->lastClosedBar(sym, BarType::Time, 60);
            (void)strategy->lastNClosedBars(sym, BarType::Time, 60, 8);
          }
        }
      });

  writer.join();
  reader.join();

  auto last = strategy->lastClosedBar(1, BarType::Time, 60);
  ASSERT_TRUE(last.has_value());
}
