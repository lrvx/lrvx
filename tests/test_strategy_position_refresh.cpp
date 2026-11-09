/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// What a market-data tick pays for the position it usually has not changed,
// and what it is told.
//
// Strategy::refreshPosition() runs on every trade, every book update and
// every bar, while the symbol lock is held. It used to make two separate
// calls into the position manager -- getPosition() and getAverageEntryPrice()
// -- and on the shipped trackers each of those takes the one mutex the
// execution thread also wants and walks the symbol's lots again. One
// question per tick instead, through positionSnapshot().
//
// IPositionManager is the seam, so the cost is countable: every acquisition
// of the tracker's mutex is one call through this interface. The answer has
// to be right as well as cheap, and for a tracker that keeps the two sides
// apart the entry price in the snapshot is the blend of them -- the same
// number getAverageEntryPrice() gives for the same state.

#include "lrvx/book/events/book_update_event.h"
#include "lrvx/book/events/trade_event.h"
#include "lrvx/engine/symbol_registry.h"
#include "lrvx/position/abstract_position_manager.h"
#include "lrvx/position/multi_mode_position_tracker.h"
#include "lrvx/position/position_tracker.h"
#include "lrvx/strategy/strategy.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

using namespace lrvx;

namespace
{

// Counts what a tick asks of the position manager. On PositionTracker each
// of these calls is one lock/unlock of the tracker mutex and one or two
// passes over the lot deque.
class CountingPositionManager : public IPositionManager
{
 public:
  CountingPositionManager() : IPositionManager(99) {}

  void start() override {}
  void stop() override {}

  Quantity getPosition(SymbolId) const override
  {
    ++queries;
    return _position;
  }

  std::optional<Price> getAverageEntryPrice(SymbolId) const override
  {
    ++queries;
    if (_position.isZero())
    {
      return std::nullopt;
    }
    return _entry;
  }

  void setFill(Quantity qty, Price entry)
  {
    _position = qty;
    _entry = entry;
  }

  mutable int queries{0};

 private:
  Quantity _position{};
  Price _entry{};
};

class CountingStrategy : public Strategy
{
 public:
  using Strategy::Strategy;

  std::vector<double> positionsSeen;
  std::vector<Quantity> rawPositionsSeen;
  std::vector<std::optional<Price>> entriesSeen;

 protected:
  void onSymbolTrade(SymbolContext& ctx, const TradeEvent&) override { record(ctx); }

  void onSymbolBook(SymbolContext& ctx, const BookUpdateEvent&) override { record(ctx); }

  void onSymbolTouch(SymbolContext& ctx, const BookUpdateEvent&) override { record(ctx); }

 private:
  void record(const SymbolContext& ctx)
  {
    positionsSeen.push_back(ctx.position.toDouble());
    rawPositionsSeen.push_back(ctx.position);
    entriesSeen.push_back(ctx.avgEntryPrice);
  }
};

Order makeFill(OrderId id, SymbolId sym, Side side, double price, double qty)
{
  Order o{};
  o.id = id;
  o.symbol = sym;
  o.side = side;
  o.price = Price::fromDouble(price);
  o.quantity = Quantity::fromDouble(qty);
  return o;
}

// The blend the tracker computes, in raw fixed point rather than through a
// double: quantity-weighted notional over total quantity, the two sides
// together.
Price blendedEntryRaw(double longQty, double longEntry, double shortQty, double shortEntry)
{
  const Volume notional =
      Volume::fromRaw((Quantity::fromDouble(longQty) * Price::fromDouble(longEntry)).raw() +
                      (Quantity::fromDouble(shortQty) * Price::fromDouble(shortEntry)).raw());
  return notional / Quantity::fromDouble(longQty + shortQty);
}

SymbolId addSymbol(SymbolRegistry& reg, const std::string& spelling)
{
  SymbolInfo info;
  info.exchange = "test";
  info.symbol = spelling;
  info.type = InstrumentType::Spot;
  info.tickSize = Price::fromDouble(0.01);
  return reg.registerSymbol(info);
}

TradeEvent makeTrade(SymbolId sym, double price, int64_t tsNs)
{
  TradeEvent ev{};
  ev.trade.symbol = sym;
  ev.trade.price = Price::fromDouble(price);
  ev.trade.quantity = Quantity::fromDouble(1.0);
  ev.trade.isBuy = true;
  ev.trade.exchangeTsNs = UnixNanos::fromRaw(tsNs);
  return ev;
}

}  // namespace

// The green control: the refresh does its job. Whatever is done about the
// cost, the context still has to carry the position the manager reports.
TEST(StrategyPositionRefresh, TheContextCarriesThePositionTheManagerReports)
{
  SymbolRegistry reg;
  const SymbolId sym = addSymbol(reg, "BTCUSDT");

  CountingPositionManager pm;
  CountingStrategy strat(1, std::vector<SymbolId>{sym}, reg);
  strat.setPositionManager(&pm);

  strat.onTrade(makeTrade(sym, 100.0, 1));
  ASSERT_EQ(strat.positionsSeen.size(), 1u);
  EXPECT_DOUBLE_EQ(strat.positionsSeen[0], 0.0);

  pm.setFill(Quantity::fromDouble(0.5), Price::fromDouble(100.0));
  strat.onTrade(makeTrade(sym, 101.0, 2));
  ASSERT_EQ(strat.positionsSeen.size(), 2u);
  EXPECT_DOUBLE_EQ(strat.positionsSeen[1], 0.5)
      << "the context must reflect the fill on the next tick";

  pm.setFill(Quantity{}, Price{});
  strat.onTrade(makeTrade(sym, 102.0, 3));
  ASSERT_EQ(strat.positionsSeen.size(), 3u);
  EXPECT_DOUBLE_EQ(strat.positionsSeen[2], 0.0) << "and the flattening after it";
}

// One tick, one question. Two calls per tick means two acquisitions of the
// tracker mutex on the market-data thread, each contending with the
// execution thread, for two values that come from the same state under the
// same lock.
//
// needs, if the answer is to ask once rather than to ask less often:
//   struct PositionSnapshot { Quantity position; std::optional<Price> avgEntryPrice; };
//   virtual PositionSnapshot IPositionManager::snapshot(SymbolId) const;
//   -- one call, one lock, one pass over the lots -- with refreshPosition()
//   calling it instead of the two getters.
TEST(StrategyPositionRefresh, ATickAsksThePositionManagerAtMostOnce)
{
  SymbolRegistry reg;
  const SymbolId sym = addSymbol(reg, "BTCUSDT");

  CountingPositionManager pm;
  CountingStrategy strat(1, std::vector<SymbolId>{sym}, reg);
  strat.setPositionManager(&pm);
  pm.setFill(Quantity::fromDouble(0.5), Price::fromDouble(100.0));

  constexpr int kTicks = 1000;
  pm.queries = 0;
  for (int i = 0; i < kTicks; ++i)
  {
    strat.onTrade(makeTrade(sym, 100.0 + i * 0.01, i + 1));
  }

  EXPECT_LE(pm.queries, kTicks)
      << kTicks << " ticks cost " << pm.queries
      << " calls into the position manager: getPosition() and "
         "getAverageEntryPrice() are asked separately, so every market-data "
         "tick takes the tracker's mutex twice and walks the lot deque twice, "
         "on the thread the execution path contends with";
}

// A top reaches the strategy through onSymbolTouch, so it refreshes the
// position like any other dispatching tick, and asks once like them: a depth
// frame that fires onSymbolBook and onSymbolTouch is still one tick.
TEST(StrategyPositionRefresh, ATouchMoveIsOneTickWithAFreshPosition)
{
  SymbolRegistry reg;
  const SymbolId sym = addSymbol(reg, "BTCUSDT");

  CountingPositionManager pm;
  CountingStrategy strat(1, std::vector<SymbolId>{sym}, reg);
  strat.setPositionManager(&pm);

  pm.queries = 0;
  strat.onTrade(makeTrade(sym, 100.0, 1));
  const int perTick = pm.queries;

  std::pmr::monotonic_buffer_resource res{4096};
  BookUpdateEvent snap(&res);
  snap.update.symbol = sym;
  snap.update.type = BookUpdateType::SNAPSHOT;
  snap.update.bids.emplace_back(Price::fromDouble(100.0), Quantity::fromDouble(1.0));
  snap.update.asks.emplace_back(Price::fromDouble(101.0), Quantity::fromDouble(1.0));
  pm.queries = 0;
  strat.onBookUpdate(snap);
  EXPECT_EQ(strat.positionsSeen.size(), 3u) << "the trade, then the book and the touch it moved";
  EXPECT_EQ(pm.queries, perTick);

  pm.setFill(Quantity::fromDouble(0.5), Price::fromDouble(100.0));
  BookUpdateEvent top(&res);
  top.update.symbol = sym;
  top.update.type = BookUpdateType::TOP_OF_BOOK;
  top.update.bids.emplace_back(Price::fromDouble(100.4), Quantity::fromDouble(1.0));
  top.update.asks.emplace_back(Price::fromDouble(100.6), Quantity::fromDouble(1.0));
  pm.queries = 0;
  strat.onBookUpdate(top);
  ASSERT_EQ(strat.positionsSeen.size(), 4u);
  EXPECT_DOUBLE_EQ(strat.positionsSeen[3], 0.5) << "onSymbolTouch sees the fill made since";
  EXPECT_EQ(pm.queries, perTick);

  // A top that leaves the touch alone dispatches nothing and asks nothing.
  pm.queries = 0;
  strat.onBookUpdate(top);
  EXPECT_EQ(strat.positionsSeen.size(), 4u);
  EXPECT_EQ(pm.queries, 0);
}

// ------------------------------------------- the other shipped tracker
//
// MultiModePositionTracker keeps the two sides apart, so "the entry price"
// is a blend of them -- which is what getAverageEntryPrice() has always
// answered, and what positionSnapshot() has to answer too, from the same
// state under the same lock. A snapshot that reported one side only would
// give a short book an entry price of zero and a mixed book the long leg's
// price, and the strategy would read unrealized PnL off it: on a short, the
// whole notional counted as profit.

TEST(MultiModePositionSnapshot, AShortOnlyBookReportsTheShortEntryNotZero)
{
  constexpr SymbolId sym = 100;
  MultiModePositionTracker tracker{7, PositionAggregationMode::NET, CostBasisMethod::FIFO};
  tracker.openShort(sym, Price::fromDouble(200.0), Quantity::fromDouble(3.0));

  const PositionSnapshot snap = tracker.positionSnapshot(sym);

  EXPECT_EQ(snap.position.raw(), Quantity::fromDouble(-3.0).raw());
  ASSERT_TRUE(snap.avgEntryPrice.has_value())
      << "a book that is short three lots has a cost basis";
  EXPECT_EQ(snap.avgEntryPrice->raw(), Price::fromDouble(200.0).raw())
      << "the snapshot reported " << snap.avgEntryPrice->toDouble()
      << " for a short opened at 200: the long side's entry, which for a "
         "short-only book is the default-constructed zero";
  EXPECT_EQ(snap.avgEntryPrice->raw(), tracker.getAverageEntryPrice(sym)->raw())
      << "positionSnapshot() and getAverageEntryPrice() must answer the same "
         "state with the same number";
}

TEST(MultiModePositionSnapshot, APerSideShortOnlyBookReportsTheShortEntry)
{
  constexpr SymbolId sym = 101;
  MultiModePositionTracker tracker{8, PositionAggregationMode::PER_SIDE, CostBasisMethod::FIFO};
  tracker.onOrderFilled(makeFill(1, sym, Side::SELL, 51000.0, 2.0));

  const PositionSnapshot snap = tracker.positionSnapshot(sym);

  EXPECT_EQ(snap.position.raw(), Quantity::fromDouble(-2.0).raw());
  ASSERT_TRUE(snap.avgEntryPrice.has_value());
  EXPECT_EQ(snap.avgEntryPrice->raw(), Price::fromDouble(51000.0).raw());
  EXPECT_EQ(snap.avgEntryPrice->raw(), tracker.getAverageEntryPrice(sym)->raw());

  const auto perSide = tracker.snapshot(sym);
  EXPECT_EQ(perSide.longAvgEntry.raw(), 0)
      << "the long side is empty here, which is exactly what makes reporting "
         "it instead of the blend invisible to a test that only looks at the "
         "position";
}

TEST(MultiModePositionSnapshot, AMixedPerSideBookReportsTheBlendOfBothSides)
{
  constexpr SymbolId sym = 102;
  MultiModePositionTracker tracker{9, PositionAggregationMode::PER_SIDE, CostBasisMethod::FIFO};
  tracker.onOrderFilled(makeFill(1, sym, Side::BUY, 50000.0, 2.0));
  tracker.onOrderFilled(makeFill(2, sym, Side::SELL, 51000.0, 1.0));

  const auto perSide = tracker.snapshot(sym);
  ASSERT_EQ(perSide.longQty.raw(), Quantity::fromDouble(2.0).raw());
  ASSERT_EQ(perSide.shortQty.raw(), Quantity::fromDouble(1.0).raw());

  const Price expected = blendedEntryRaw(2.0, 50000.0, 1.0, 51000.0);
  const PositionSnapshot snap = tracker.positionSnapshot(sym);

  EXPECT_EQ(snap.position.raw(), Quantity::fromDouble(1.0).raw()) << "net of the two sides";
  ASSERT_TRUE(snap.avgEntryPrice.has_value());
  EXPECT_EQ(snap.avgEntryPrice->raw(), expected.raw())
      << "the snapshot reported " << snap.avgEntryPrice->toDouble() << ", the blend is "
      << expected.toDouble();
  EXPECT_NE(snap.avgEntryPrice->raw(), perSide.longAvgEntry.raw())
      << "the long leg's own entry price is not the book's cost basis when "
         "there is a short leg as well";
  EXPECT_EQ(snap.avgEntryPrice->raw(), tracker.getAverageEntryPrice(sym)->raw())
      << "the two answers for one state must not drift apart";
}

TEST(MultiModePositionSnapshot, AFlatBookReportsNoEntryPriceAtAll)
{
  constexpr SymbolId sym = 103;
  MultiModePositionTracker tracker{10, PositionAggregationMode::PER_SIDE, CostBasisMethod::FIFO};

  const PositionSnapshot snap = tracker.positionSnapshot(sym);
  EXPECT_TRUE(snap.position.isZero());
  EXPECT_FALSE(snap.avgEntryPrice.has_value())
      << "a flat book has no entry price, and a zero would read as a real one";
}

// The same thing where it is actually read: through the strategy, on a tick,
// with the multi-mode tracker attached as the position manager.
TEST(StrategyPositionRefresh, AMultiModeTrackerReachesTheContextThroughTheSnapshot)
{
  SymbolRegistry reg;
  const SymbolId sym = addSymbol(reg, "BTCUSDT");

  MultiModePositionTracker tracker{11, PositionAggregationMode::PER_SIDE,
                                   CostBasisMethod::FIFO};
  CountingStrategy strat(1, std::vector<SymbolId>{sym}, reg);
  strat.setPositionManager(&tracker);

  // Short only first: the case where reporting the long side would hand the
  // strategy a zero cost basis and turn the whole notional into unrealized
  // profit.
  tracker.onOrderFilled(makeFill(1, sym, Side::SELL, 51000.0, 2.0));
  strat.onTrade(makeTrade(sym, 50900.0, 1));

  ASSERT_EQ(strat.entriesSeen.size(), 1u);
  EXPECT_EQ(strat.rawPositionsSeen[0].raw(), Quantity::fromDouble(-2.0).raw());
  ASSERT_TRUE(strat.entriesSeen[0].has_value()) << "the context was given no cost basis";
  EXPECT_EQ(strat.entriesSeen[0]->raw(), Price::fromDouble(51000.0).raw())
      << "ctx.avgEntryPrice came back as " << strat.entriesSeen[0]->toDouble();

  // Then mixed: a long leg opens alongside the short one.
  tracker.onOrderFilled(makeFill(2, sym, Side::BUY, 50000.0, 3.0));
  strat.onTrade(makeTrade(sym, 50900.0, 2));

  ASSERT_EQ(strat.entriesSeen.size(), 2u);
  EXPECT_EQ(strat.rawPositionsSeen[1].raw(), Quantity::fromDouble(1.0).raw());
  ASSERT_TRUE(strat.entriesSeen[1].has_value());
  EXPECT_EQ(strat.entriesSeen[1]->raw(), blendedEntryRaw(3.0, 50000.0, 2.0, 51000.0).raw());
  EXPECT_EQ(strat.entriesSeen[1]->raw(), tracker.getAverageEntryPrice(sym)->raw());
}

// ------------------------------------------- the lot-based tracker
//
// PositionTracker keeps a deque of lots rather than a long and a short side,
// and PositionState::snapshot() answers position and cost basis from one pass
// over it. The cost basis weighs every lot by its magnitude -- a short lot
// weighs what a long one of the same size weighs, which is what
// WeightedPriceSum does for avgEntryPrice() and what a cost basis means.
// Dropping that magnitude leaves the answer unchanged for any book whose lots
// all point the same way, because the sign then cancels between the numerator
// and the denominator; it shows up only where the book holds both
// directions. The same is true of the "nothing when flat" guard: a book with
// no lots at all is caught by the total quantity, and only a book whose lots
// net to zero while still weighing something needs the position term.
//
// The fill path never builds such a book -- a closing fill closes the
// existing lots before the remainder opens one the other way -- so these are
// built on PositionState directly, which is the type the function belongs to.

TEST(PositionStateSnapshot, EveryLotWeighsItsMagnitudeWhicheverWayItPoints)
{
  PositionState state;
  state.lots = {{Quantity::fromDouble(2.0), Price::fromDouble(100.0)},
                {Quantity::fromDouble(-1.0), Price::fromDouble(200.0)}};

  const PositionSnapshot snap = state.snapshot();

  EXPECT_EQ(snap.position.raw(), Quantity::fromDouble(1.0).raw())
      << "the position is the signed sum, two long against one short";
  ASSERT_TRUE(snap.avgEntryPrice.has_value());
  EXPECT_EQ(snap.avgEntryPrice->raw(), state.avgEntryPrice().raw())
      << "the snapshot reported " << snap.avgEntryPrice->toDouble()
      << " where the weighted average of the same lots is "
      << state.avgEntryPrice().toDouble()
      << ": the short lot was weighed as a negative quantity instead of as "
         "one lot of that size, so it cancelled the long lot's notional "
         "instead of adding to it";
  EXPECT_GT(snap.avgEntryPrice->raw(), Price::fromDouble(100.0).raw())
      << "the cost basis lies between the two prices paid";
  EXPECT_LT(snap.avgEntryPrice->raw(), Price::fromDouble(200.0).raw());
}

TEST(PositionStateSnapshot, LotsThatNetToZeroReportNoEntryPriceAtAll)
{
  PositionState state;
  state.lots = {{Quantity::fromDouble(2.0), Price::fromDouble(100.0)},
                {Quantity::fromDouble(-2.0), Price::fromDouble(200.0)}};

  const PositionSnapshot snap = state.snapshot();

  EXPECT_TRUE(snap.position.isZero());
  EXPECT_FALSE(snap.avgEntryPrice.has_value())
      << "the lots weigh four units between them but net to no position, and "
         "the snapshot answered with an entry price of "
      << (snap.avgEntryPrice ? snap.avgEntryPrice->toDouble() : 0.0)
      << ": a flat book has no cost basis, and a number there reads as one";

  // A book with no lots at all is the ordinary way to be flat -- opened and
  // then fully closed -- and it is the total quantity, not the position, that
  // answers for it. Both guards are needed; neither covers the other.
  PositionState closed;
  const PositionSnapshot empty = closed.snapshot();
  EXPECT_TRUE(empty.position.isZero());
  EXPECT_FALSE(empty.avgEntryPrice.has_value());
}

TEST(PositionTrackerSnapshot, AShortBookReportsANegativePositionAndTheShortsEntry)
{
  constexpr SymbolId sym = 200;
  PositionTracker tracker{12, CostBasisMethod::FIFO};

  tracker.onOrderFilled(makeFill(1, sym, Side::SELL, 100.0, 2.0));
  tracker.onOrderFilled(makeFill(2, sym, Side::SELL, 200.0, 2.0));

  const PositionSnapshot snap = tracker.positionSnapshot(sym);

  EXPECT_EQ(snap.position.raw(), Quantity::fromDouble(-4.0).raw())
      << "four units sold with nothing to sell is a short of four";
  ASSERT_TRUE(snap.avgEntryPrice.has_value()) << "a short has a cost basis like any position";
  EXPECT_EQ(snap.avgEntryPrice->raw(), Price::fromDouble(150.0).raw())
      << "two at 100 and two at 200 average to 150; the snapshot said "
      << snap.avgEntryPrice->toDouble();
  EXPECT_EQ(snap.avgEntryPrice->raw(), tracker.getAverageEntryPrice(sym)->raw())
      << "the snapshot and the query must answer one state with one number";
  EXPECT_EQ(snap.position.raw(), tracker.getPosition(sym).raw());
}

// The same short, where a strategy reads it.
TEST(StrategyPositionRefresh, ALotTrackersShortReachesTheContextThroughTheSnapshot)
{
  SymbolRegistry reg;
  const SymbolId sym = addSymbol(reg, "BTCUSDT");

  PositionTracker tracker{13, CostBasisMethod::FIFO};
  CountingStrategy strat(1, std::vector<SymbolId>{sym}, reg);
  strat.setPositionManager(&tracker);

  tracker.onOrderFilled(makeFill(1, sym, Side::SELL, 100.0, 2.0));
  tracker.onOrderFilled(makeFill(2, sym, Side::SELL, 200.0, 2.0));
  strat.onTrade(makeTrade(sym, 140.0, 1));

  ASSERT_EQ(strat.entriesSeen.size(), 1u);
  EXPECT_EQ(strat.rawPositionsSeen[0].raw(), Quantity::fromDouble(-4.0).raw())
      << "ctx.position must carry the sign: a short that reads as flat or "
         "long is a strategy that never covers";
  ASSERT_TRUE(strat.entriesSeen[0].has_value()) << "the context was given no cost basis";
  EXPECT_EQ(strat.entriesSeen[0]->raw(), Price::fromDouble(150.0).raw())
      << "ctx.avgEntryPrice came back as " << strat.entriesSeen[0]->toDouble();

  // Cover half of it and the remaining short still reports the entry price of
  // the lots that are left.
  tracker.onOrderFilled(makeFill(3, sym, Side::BUY, 120.0, 2.0));
  strat.onTrade(makeTrade(sym, 140.0, 2));

  ASSERT_EQ(strat.entriesSeen.size(), 2u);
  EXPECT_EQ(strat.rawPositionsSeen[1].raw(), Quantity::fromDouble(-2.0).raw());
  ASSERT_TRUE(strat.entriesSeen[1].has_value());
  EXPECT_EQ(strat.entriesSeen[1]->raw(), Price::fromDouble(200.0).raw())
      << "FIFO covered the lot sold at 100 first, so what is left was sold at 200";
}

// positionSnapshot() and getAverageEntryPrice() share WeightedPriceSum, so
// on a book whose average is not representable -- two at 100 and four at 200,
// 166.666666... -- the two answers for one state are the same to the last raw
// unit. A snapshot that accumulated in double and divided once landed a raw
// unit away from the query on exactly this book.
TEST(PositionTrackerSnapshot, TheSnapshotAndTheQueryAgreeToTheLastRaw)
{
  constexpr SymbolId sym = 201;
  PositionTracker tracker{14, CostBasisMethod::FIFO};

  tracker.onOrderFilled(makeFill(1, sym, Side::SELL, 100.0, 2.0));
  tracker.onOrderFilled(makeFill(2, sym, Side::SELL, 200.0, 4.0));

  const PositionSnapshot snap = tracker.positionSnapshot(sym);
  const auto queried = tracker.getAverageEntryPrice(sym);

  ASSERT_TRUE(snap.avgEntryPrice.has_value());
  ASSERT_TRUE(queried.has_value());
  EXPECT_EQ(snap.avgEntryPrice->raw(), queried->raw())
      << "the snapshot said " << snap.avgEntryPrice->toDouble() << " and the query said "
      << queried->toDouble() << " for the same lots";
  EXPECT_EQ(snap.position.raw(), Quantity::fromDouble(-6.0).raw());
}
