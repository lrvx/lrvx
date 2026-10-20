/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "lrvx/book/events/book_update_event.h"
#include "lrvx/book/nlevel_order_book.h"
#include "lrvx/common.h"

#include <gtest/gtest.h>

using namespace lrvx;

class NLevelOrderBookTest : public ::testing::Test
{
 protected:
  NLevelOrderBook<> book{Price::fromDouble(0.1)};
  using BookUpdatePool = pool::Pool<BookUpdateEvent, 63>;
  BookUpdatePool pool;

  pool::Handle<BookUpdateEvent> makeSnapshot(const std::vector<BookLevel>& bids,
                                             const std::vector<BookLevel>& asks)
  {
    auto opt = pool.acquire();
    assert(opt);
    auto& u = *opt;
    u->update.type = BookUpdateType::SNAPSHOT;
    u->update.bids.assign(bids.begin(), bids.end());
    u->update.asks.assign(asks.begin(), asks.end());
    return std::move(u);
  }

  pool::Handle<BookUpdateEvent> makeDelta(const std::vector<BookLevel>& bids,
                                          const std::vector<BookLevel>& asks)
  {
    auto opt = pool.acquire();
    assert(opt);
    auto& u = *opt;
    u->update.type = BookUpdateType::DELTA;
    u->update.bids.assign(bids.begin(), bids.end());
    u->update.asks.assign(asks.begin(), asks.end());
    return std::move(u);
  }
};

TEST_F(NLevelOrderBookTest, AppliesSnapshotCorrectly)
{
  auto update = makeSnapshot({{Price::fromDouble(100.0), Quantity::fromDouble(2.0)},
                              {Price::fromDouble(99.0), Quantity::fromDouble(1.0)}},
                             {{Price::fromDouble(101.0), Quantity::fromDouble(1.5)},
                              {Price::fromDouble(102.0), Quantity::fromDouble(3.0)}});
  book.applyBookUpdate(*update);

  EXPECT_EQ(book.bestBid(), Price::fromDouble(100.0));
  EXPECT_EQ(book.bestAsk(), Price::fromDouble(101.0));

  EXPECT_EQ(book.bidAtPrice(Price::fromDouble(100.0)), Quantity::fromDouble(2.0));
  EXPECT_EQ(book.bidAtPrice(Price::fromDouble(99.0)), Quantity::fromDouble(1.0));
  EXPECT_EQ(book.askAtPrice(Price::fromDouble(101.0)), Quantity::fromDouble(1.5));
  EXPECT_EQ(book.askAtPrice(Price::fromDouble(102.0)), Quantity::fromDouble(3.0));
}

TEST_F(NLevelOrderBookTest, IgnoresTopOfBookFrames)
{
  auto snap = makeSnapshot({{Price::fromDouble(100.0), Quantity::fromDouble(2.0)},
                            {Price::fromDouble(99.0), Quantity::fromDouble(1.0)}},
                           {{Price::fromDouble(101.0), Quantity::fromDouble(1.5)}});
  book.applyBookUpdate(*snap);

  // A TOP_OF_BOOK frame carries the touch, not the book: merged as a delta
  // it would strand 100.0 as a stale best when the top moved to 99.5,
  // applied as a snapshot it would drop the depth. The book must change
  // nothing.
  auto top = makeSnapshot({{Price::fromDouble(99.5), Quantity::fromDouble(4.0)}},
                          {{Price::fromDouble(100.5), Quantity::fromDouble(1.0)}});
  top->update.type = BookUpdateType::TOP_OF_BOOK;
  book.applyBookUpdate(*top);

  EXPECT_EQ(book.bestBid(), Price::fromDouble(100.0));
  EXPECT_EQ(book.bestAsk(), Price::fromDouble(101.0));
  EXPECT_EQ(book.bidAtPrice(Price::fromDouble(99.0)), Quantity::fromDouble(1.0));
  EXPECT_EQ(book.bidAtPrice(Price::fromDouble(99.5)), Quantity::fromDouble(0.0));
}

TEST_F(NLevelOrderBookTest, AppliesDeltaCorrectly)
{
  auto snap = makeSnapshot({{Price::fromDouble(100.0), Quantity::fromDouble(1.0)}},
                           {{Price::fromDouble(101.0), Quantity::fromDouble(2.0)}});
  book.applyBookUpdate(*snap);

  auto delta = makeDelta({{Price::fromDouble(100.0), Quantity::fromDouble(0.0)},
                          {Price::fromDouble(99.0), Quantity::fromDouble(1.5)}},
                         {{Price::fromDouble(101.0), Quantity::fromDouble(3.0)}});
  book.applyBookUpdate(*delta);

  EXPECT_EQ(book.bestBid(), Price::fromDouble(99.0));
  EXPECT_EQ(book.bestAsk(), Price::fromDouble(101.0));

  EXPECT_EQ(book.bidAtPrice(Price::fromDouble(99.0)), Quantity::fromDouble(1.5));
  EXPECT_EQ(book.bidAtPrice(Price::fromDouble(100.0)), Quantity::fromDouble(0.0));
  EXPECT_EQ(book.askAtPrice(Price::fromDouble(101.0)), Quantity::fromDouble(3.0));
}

TEST_F(NLevelOrderBookTest, HandlesEmptyBook)
{
  EXPECT_EQ(book.bestBid(), std::nullopt);
  EXPECT_EQ(book.bestAsk(), std::nullopt);
  EXPECT_EQ(book.bidAtPrice(Price::fromDouble(123.0)), Quantity::fromDouble(0.0));
  EXPECT_EQ(book.askAtPrice(Price::fromDouble(123.0)), Quantity::fromDouble(0.0));
}

static inline void ExpectPairNear(const std::pair<Quantity, Volume>& got,
                                  double expFilled, double expNotional,
                                  double eps = 1e-6)
{
  EXPECT_NEAR(got.first.toDouble(), expFilled, eps);
  EXPECT_NEAR(got.second.toDouble(), expNotional, eps);
}

TEST_F(NLevelOrderBookTest, ConsumeAsks_Basic)
{
  auto up = makeSnapshot(
      {},
      {{Price::fromDouble(100.0), Quantity::fromDouble(1.0)},
       {Price::fromDouble(100.1), Quantity::fromDouble(2.0)},
       {Price::fromDouble(100.2), Quantity::fromDouble(3.0)}});

  book.applyBookUpdate(*up);

  ExpectPairNear(book.consumeAsks(Quantity::fromDouble(0.0)), 0.0, 0.0);
  ExpectPairNear(book.consumeAsks(Quantity::fromDouble(1.0)), 1.0, 100.0);
  ExpectPairNear(book.consumeAsks(Quantity::fromDouble(2.5)), 2.5, 250.15);
  ExpectPairNear(book.consumeAsks(Quantity::fromDouble(10.0)), 6.0, 600.8);
}

TEST_F(NLevelOrderBookTest, ConsumeBids_Basic)
{
  auto up = makeSnapshot(
      {{Price::fromDouble(100.0), Quantity::fromDouble(1.0)},
       {Price::fromDouble(99.9), Quantity::fromDouble(2.0)},
       {Price::fromDouble(99.8), Quantity::fromDouble(3.0)}},
      {});

  book.applyBookUpdate(*up);

  ExpectPairNear(book.consumeBids(Quantity::fromDouble(2.5)), 2.5, 249.85);
  ExpectPairNear(book.consumeBids(Quantity::fromDouble(10.0)), 6.0, 599.2);
}

TEST_F(NLevelOrderBookTest, ConsumeAsks_WithHoles)
{
  auto up = makeSnapshot(
      {},
      {{Price::fromDouble(100.0), Quantity::fromDouble(0.0)},
       {Price::fromDouble(100.1), Quantity::fromDouble(2.0)},
       {Price::fromDouble(100.2), Quantity::fromDouble(0.0)},
       {Price::fromDouble(100.3), Quantity::fromDouble(3.0)}});

  book.applyBookUpdate(*up);

  ExpectPairNear(book.consumeAsks(Quantity::fromDouble(2.0)), 2.0, 200.2);
  ExpectPairNear(book.consumeAsks(Quantity::fromDouble(4.0)), 4.0, 400.8);
  ExpectPairNear(book.consumeAsks(Quantity::fromDouble(10.0)), 5.0, 501.1);
}

TEST_F(NLevelOrderBookTest, ConsumeBids_WithHoles)
{
  auto up = makeSnapshot(
      {{Price::fromDouble(100.0), Quantity::fromDouble(0.0)},
       {Price::fromDouble(99.9), Quantity::fromDouble(2.0)},
       {Price::fromDouble(99.8), Quantity::fromDouble(0.0)},
       {Price::fromDouble(99.7), Quantity::fromDouble(3.0)}},
      {});

  book.applyBookUpdate(*up);

  ExpectPairNear(book.consumeBids(Quantity::fromDouble(3.0)), 3.0, 299.5);
  ExpectPairNear(book.consumeBids(Quantity::fromDouble(10.0)), 5.0, 498.9);
}

TEST_F(NLevelOrderBookTest, Consume_EmptyBook)
{
  auto up = makeSnapshot({}, {});
  book.applyBookUpdate(*up);

  ExpectPairNear(book.consumeAsks(Quantity::fromDouble(5.0)), 0.0, 0.0);
  ExpectPairNear(book.consumeBids(Quantity::fromDouble(5.0)), 0.0, 0.0);
}

TEST_F(NLevelOrderBookTest, Consume_IsConstDoesNotMutate)
{
  auto up = makeSnapshot(
      {{Price::fromDouble(100.0), Quantity::fromDouble(1.0)},
       {Price::fromDouble(99.9), Quantity::fromDouble(2.0)}},
      {{Price::fromDouble(100.1), Quantity::fromDouble(2.0)},
       {Price::fromDouble(100.2), Quantity::fromDouble(3.0)}});

  book.applyBookUpdate(*up);

  auto r1 = book.consumeAsks(Quantity::fromDouble(3.5));
  auto r2 = book.consumeAsks(Quantity::fromDouble(3.5));
  ExpectPairNear(r1, r2.first.toDouble(), r2.second.toDouble());

  auto b1 = book.consumeBids(Quantity::fromDouble(2.25));
  auto b2 = book.consumeBids(Quantity::fromDouble(2.25));
  ExpectPairNear(b1, b2.first.toDouble(), b2.second.toDouble());

  EXPECT_EQ(book.bestAsk(), Price::fromDouble(100.1));
  EXPECT_EQ(book.bestBid(), Price::fromDouble(100.0));
}

TEST_F(NLevelOrderBookTest, SpreadCalculation)
{
  auto up = makeSnapshot({{Price::fromDouble(100.0), Quantity::fromDouble(1.0)}},
                         {{Price::fromDouble(100.5), Quantity::fromDouble(1.0)}});
  book.applyBookUpdate(*up);

  auto spreadOpt = book.spread();
  ASSERT_TRUE(spreadOpt.has_value());
  EXPECT_NEAR(spreadOpt->toDouble(), 0.5, 1e-9);
}

TEST_F(NLevelOrderBookTest, MidCalculation)
{
  auto up = makeSnapshot({{Price::fromDouble(100.0), Quantity::fromDouble(1.0)}},
                         {{Price::fromDouble(101.0), Quantity::fromDouble(1.0)}});
  book.applyBookUpdate(*up);

  auto midOpt = book.mid();
  ASSERT_TRUE(midOpt.has_value());
  EXPECT_NEAR(midOpt->toDouble(), 100.5, 1e-9);
}

TEST_F(NLevelOrderBookTest, IsCrossedReturnsFalseForNormalBook)
{
  auto up = makeSnapshot({{Price::fromDouble(100.0), Quantity::fromDouble(1.0)}},
                         {{Price::fromDouble(100.1), Quantity::fromDouble(1.0)}});
  book.applyBookUpdate(*up);

  EXPECT_FALSE(book.isCrossed());
}

TEST_F(NLevelOrderBookTest, IsCrossedReturnsTrueForCrossedBook)
{
  // First set up a normal book
  auto up1 = makeSnapshot({{Price::fromDouble(100.0), Quantity::fromDouble(1.0)}},
                          {{Price::fromDouble(100.1), Quantity::fromDouble(1.0)}});
  book.applyBookUpdate(*up1);

  // Then apply a delta that crosses it (bid > ask)
  auto up2 = makeDelta({{Price::fromDouble(100.2), Quantity::fromDouble(1.0)}}, {});
  book.applyBookUpdate(*up2);

  EXPECT_TRUE(book.isCrossed());
}

TEST_F(NLevelOrderBookTest, SpreadAndMidReturnNulloptForEmptyBook)
{
  EXPECT_FALSE(book.spread().has_value());
  EXPECT_FALSE(book.mid().has_value());
  EXPECT_FALSE(book.isCrossed());
}

TEST_F(NLevelOrderBookTest, SpreadAndMidReturnNulloptForOneSidedBook)
{
  auto up = makeSnapshot({{Price::fromDouble(100.0), Quantity::fromDouble(1.0)}}, {});
  book.applyBookUpdate(*up);

  EXPECT_FALSE(book.spread().has_value());
  EXPECT_FALSE(book.mid().has_value());
  EXPECT_FALSE(book.isCrossed());
}
