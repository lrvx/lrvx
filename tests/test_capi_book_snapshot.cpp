/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// LrvxBookSnapshot carries a price and a size for each side of the top of
// book. The price fields were filled from the book; the two size fields
// were hardcoded to 0 on every event and every context, under a comment
// saying the size at the best level was "not directly available from
// bestBid()". It is: NLevelOrderBook answers bidAtPrice/askAtPrice, and the
// best price is the argument. A direct C-ABI or Codon consumer reading
// bid_qty_raw therefore saw a permanent zero and could not tell it from a
// genuinely empty side -- the three high-level bindings only avoided the
// trap by not surfacing the fields at all.
//
// These tests pin the fields to the size at the best level of the book the
// event was delivered against, and 0 only where the side really is empty.

#include "lrvx/book/events/book_update_event.h"
#include "lrvx/capi/bridge_strategy.h"
#include "lrvx/common.h"
#include "lrvx/engine/symbol_registry.h"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

using namespace lrvx;

namespace
{

struct Captured
{
  int bookCalls = 0;
  LrvxBookSnapshot fromEvent{};
  LrvxBookSnapshot fromContext{};
};

void onBookCapture(void* user, const LrvxSymbolContext* ctx, const LrvxBookData* book)
{
  auto* c = static_cast<Captured*>(user);
  ++c->bookCalls;
  c->fromEvent = book->snapshot;
  c->fromContext = ctx->book;
}

struct TouchCaptured
{
  int bookCalls = 0;
  int tradeCalls = 0;
  LrvxBookSnapshot tradeContext{};
  int64_t lastUpdateNs = 0;
  int64_t lastTouchNs = 0;
};

void onBookCount(void* user, const LrvxSymbolContext*, const LrvxBookData*)
{
  ++static_cast<TouchCaptured*>(user)->bookCalls;
}

void onTradeTouch(void* user, const LrvxSymbolContext* ctx, const LrvxTradeData*)
{
  auto* c = static_cast<TouchCaptured*>(user);
  ++c->tradeCalls;
  c->tradeContext = ctx->book;
  c->lastUpdateNs = ctx->last_update_ns;
  c->lastTouchNs = ctx->last_touch_ns;
}

SymbolId addSymbol(SymbolRegistry& reg)
{
  SymbolInfo info;
  info.exchange = "test";
  info.symbol = "BTCUSDT";
  info.type = InstrumentType::Spot;
  info.tickSize = Price::fromDouble(0.01);
  return reg.registerSymbol(info);
}

using BookUpdatePool = pool::Pool<BookUpdateEvent, 8>;

pool::Handle<BookUpdateEvent> makeSnapshot(BookUpdatePool& pool, SymbolId sym,
                                           const std::vector<BookLevel>& bids,
                                           const std::vector<BookLevel>& asks)
{
  auto opt = pool.acquire();
  EXPECT_TRUE(opt.has_value());
  auto& u = *opt;
  u->update.symbol = sym;
  u->update.type = BookUpdateType::SNAPSHOT;
  u->update.exchangeTsNs = UnixNanos{1'776'606'960'123'456'789LL};
  u->update.bids.assign(bids.begin(), bids.end());
  u->update.asks.assign(asks.begin(), asks.end());
  return std::move(u);
}

}  // namespace

TEST(CApiBookSnapshot, BestLevelSizesReachTheCConsumer)
{
  SymbolRegistry registry;
  SymbolId sym = addSymbol(registry);

  Captured captured;
  LrvxStrategyCallbacks cb{};
  cb.on_book = &onBookCapture;
  cb.user_data = &captured;

  auto bridge = std::make_unique<BridgeStrategy>(1, std::vector<SymbolId>{sym}, registry, cb);

  BookUpdatePool pool;
  auto update = makeSnapshot(pool, sym,
                             {{Price::fromDouble(100.00), Quantity::fromDouble(2.5)},
                              {Price::fromDouble(99.99), Quantity::fromDouble(7.0)}},
                             {{Price::fromDouble(100.05), Quantity::fromDouble(1.25)},
                              {Price::fromDouble(100.06), Quantity::fromDouble(9.0)}});
  bridge->onBookUpdate(*update);

  ASSERT_EQ(captured.bookCalls, 1);

  EXPECT_EQ(captured.fromEvent.bid_price_raw, Price::fromDouble(100.00).raw());
  EXPECT_EQ(captured.fromEvent.ask_price_raw, Price::fromDouble(100.05).raw());

  EXPECT_EQ(captured.fromEvent.bid_qty_raw, Quantity::fromDouble(2.5).raw())
      << "the size resting at the best bid, not the size of the whole side";
  EXPECT_EQ(captured.fromEvent.ask_qty_raw, Quantity::fromDouble(1.25).raw())
      << "the size resting at the best ask";
  EXPECT_EQ(captured.fromEvent.has_bid, 1);
  EXPECT_EQ(captured.fromEvent.has_ask, 1);

  // The same snapshot is embedded in the symbol context handed to every
  // callback, and it is built by the same helper.
  EXPECT_EQ(captured.fromContext.bid_qty_raw, Quantity::fromDouble(2.5).raw());
  EXPECT_EQ(captured.fromContext.ask_qty_raw, Quantity::fromDouble(1.25).raw());
}

TEST(CApiBookSnapshot, TopOfBookRefreshesTheTouchWithoutABookCallback)
{
  SymbolRegistry registry;
  SymbolId sym = addSymbol(registry);

  TouchCaptured cap;
  LrvxStrategyCallbacks cb{};
  cb.on_book = &onBookCount;
  cb.on_trade = &onTradeTouch;
  cb.user_data = &cap;

  auto bridge = std::make_unique<BridgeStrategy>(1, std::vector<SymbolId>{sym}, registry, cb);

  BookUpdatePool pool;
  auto snap = makeSnapshot(pool, sym, {{Price::fromDouble(100.00), Quantity::fromDouble(2.5)}},
                           {{Price::fromDouble(100.05), Quantity::fromDouble(1.25)}});
  bridge->onBookUpdate(*snap);
  ASSERT_EQ(cap.bookCalls, 1);

  // A TOP_OF_BOOK frame must not fire on_book — the depth book ignored it —
  // but it must land in the context's touch, which every later callback
  // serializes.
  auto top = makeSnapshot(pool, sym, {{Price::fromDouble(100.02), Quantity::fromDouble(4.0)}},
                          {{Price::fromDouble(100.03), Quantity::fromDouble(3.0)}});
  top->update.type = BookUpdateType::TOP_OF_BOOK;
  top->update.exchangeTsNs = UnixNanos{1'776'606'961'000'000'000LL};
  bridge->onBookUpdate(*top);
  EXPECT_EQ(cap.bookCalls, 1);

  TradeEvent tr;
  tr.trade.symbol = sym;
  tr.trade.price = Price::fromDouble(100.02);
  tr.trade.quantity = Quantity::fromDouble(0.1);
  tr.trade.exchangeTsNs = UnixNanos{1'776'606'962'000'000'000LL};
  bridge->onTrade(tr);

  ASSERT_EQ(cap.tradeCalls, 1);
  EXPECT_EQ(cap.tradeContext.has_bid, 1);
  EXPECT_EQ(cap.tradeContext.bid_price_raw, Price::fromDouble(100.02).raw())
      << "the touch from the TOP_OF_BOOK frame, not the stale snapshot top";
  EXPECT_EQ(cap.tradeContext.bid_qty_raw, Quantity::fromDouble(4.0).raw());
  EXPECT_EQ(cap.tradeContext.ask_price_raw, Price::fromDouble(100.03).raw());
  EXPECT_EQ(cap.tradeContext.ask_qty_raw, Quantity::fromDouble(3.0).raw());

  // The pull accessors must agree with the snapshot the callbacks carry:
  // Codon and QuickJS strategies read the top through these.
  auto* h = static_cast<LrvxStrategyHandle>(bridge.get());
  int64_t px = 0;
  ASSERT_EQ(lrvx_best_bid_raw_opt(h, sym, &px), 1);
  EXPECT_EQ(px, Price::fromDouble(100.02).raw());
  ASSERT_EQ(lrvx_best_ask_raw_opt(h, sym, &px), 1);
  EXPECT_EQ(px, Price::fromDouble(100.03).raw());
  ASSERT_EQ(lrvx_mid_price_raw_opt(h, sym, &px), 1);
  EXPECT_EQ(px, Price::fromDouble(100.025).raw());
  EXPECT_EQ(lrvx_best_bid_raw(h, sym), Price::fromDouble(100.02).raw());
  EXPECT_EQ(lrvx_best_ask_raw(h, sym), Price::fromDouble(100.03).raw());
  EXPECT_EQ(lrvx_mid_price_raw(h, sym), Price::fromDouble(100.025).raw());

  // The two stamps answer different questions: last_update_ns is the last
  // dispatched event (the trade), last_touch_ns the TOP_OF_BOOK frame.
  EXPECT_EQ(cap.lastUpdateNs, 1'776'606'962'000'000'000LL);
  EXPECT_EQ(cap.lastTouchNs, 1'776'606'961'000'000'000LL);

  LrvxSymbolContext fctx{};
  lrvx_get_symbol_context(h, sym, &fctx);
  EXPECT_EQ(fctx.last_update_ns, 1'776'606'962'000'000'000LL);
  EXPECT_EQ(fctx.last_touch_ns, 1'776'606'961'000'000'000LL);
}

TEST(CApiBookSnapshot, EmptySideReportsZeroPriceAndZeroSize)
{
  SymbolRegistry registry;
  SymbolId sym = addSymbol(registry);

  Captured captured;
  LrvxStrategyCallbacks cb{};
  cb.on_book = &onBookCapture;
  cb.user_data = &captured;

  auto bridge = std::make_unique<BridgeStrategy>(1, std::vector<SymbolId>{sym}, registry, cb);

  BookUpdatePool pool;
  auto update =
      makeSnapshot(pool, sym, {{Price::fromDouble(100.00), Quantity::fromDouble(3.0)}}, {});
  bridge->onBookUpdate(*update);

  ASSERT_EQ(captured.bookCalls, 1);

  EXPECT_EQ(captured.fromEvent.bid_qty_raw, Quantity::fromDouble(3.0).raw());
  EXPECT_EQ(captured.fromEvent.ask_price_raw, 0)
      << "no ask side, so there is no best ask price";
  EXPECT_EQ(captured.fromEvent.ask_qty_raw, 0)
      << "0 means the book cannot say, which here is the truth";
  EXPECT_EQ(captured.fromEvent.has_bid, 1) << "the bid side has a best level";
  EXPECT_EQ(captured.fromEvent.has_ask, 0)
      << "the ask side has none; the flag, not the zero price, says so";
}

// The mirror case: an ask and no bid. A flag derived from the other side
// (has_ask written from the bid) survives every two-sided and bid-only book;
// only a book with an ask alone tells the flags apart.
TEST(CApiBookSnapshot, EmptyBidSideReportsTheAskFlagAlone)
{
  SymbolRegistry registry;
  SymbolId sym = addSymbol(registry);

  Captured captured;
  LrvxStrategyCallbacks cb{};
  cb.on_book = &onBookCapture;
  cb.user_data = &captured;

  auto bridge = std::make_unique<BridgeStrategy>(1, std::vector<SymbolId>{sym}, registry, cb);

  BookUpdatePool pool;
  auto update =
      makeSnapshot(pool, sym, {}, {{Price::fromDouble(100.05), Quantity::fromDouble(1.5)}});
  bridge->onBookUpdate(*update);

  ASSERT_EQ(captured.bookCalls, 1);

  EXPECT_EQ(captured.fromEvent.has_ask, 1) << "the ask side has a best level";
  EXPECT_EQ(captured.fromEvent.ask_price_raw, Price::fromDouble(100.05).raw());
  EXPECT_EQ(captured.fromEvent.ask_qty_raw, Quantity::fromDouble(1.5).raw());
  EXPECT_EQ(captured.fromEvent.has_bid, 0) << "no bid side";
  EXPECT_EQ(captured.fromEvent.bid_price_raw, 0);
}

// The best level moving is the case a cached or hardcoded size gets wrong
// most quietly: the price follows the book and the size does not.
TEST(CApiBookSnapshot, BestLevelSizeFollowsTheBookAcrossUpdates)
{
  SymbolRegistry registry;
  SymbolId sym = addSymbol(registry);

  Captured captured;
  LrvxStrategyCallbacks cb{};
  cb.on_book = &onBookCapture;
  cb.user_data = &captured;

  auto bridge = std::make_unique<BridgeStrategy>(1, std::vector<SymbolId>{sym}, registry, cb);

  BookUpdatePool pool;
  auto first = makeSnapshot(pool, sym,
                            {{Price::fromDouble(100.00), Quantity::fromDouble(2.5)},
                             {Price::fromDouble(99.99), Quantity::fromDouble(7.0)}},
                            {{Price::fromDouble(100.05), Quantity::fromDouble(1.25)}});
  bridge->onBookUpdate(*first);
  ASSERT_EQ(captured.fromEvent.bid_qty_raw, Quantity::fromDouble(2.5).raw());

  // Best bid pulled; 99.99 becomes the top of the bid side.
  auto second = makeSnapshot(pool, sym,
                             {{Price::fromDouble(99.99), Quantity::fromDouble(7.0)}},
                             {{Price::fromDouble(100.05), Quantity::fromDouble(1.25)}});
  bridge->onBookUpdate(*second);

  ASSERT_EQ(captured.bookCalls, 2);
  EXPECT_EQ(captured.fromEvent.bid_price_raw, Price::fromDouble(99.99).raw());
  EXPECT_EQ(captured.fromEvent.bid_qty_raw, Quantity::fromDouble(7.0).raw())
      << "the size reported is the one at the new best level";
}

// The same snapshot is reachable a second way: lrvx_get_symbol_context
// builds a LrvxSymbolContext on demand, for a C consumer that wants the
// book between events rather than inside a callback. It has its own copy
// of the best-level lookup, so it needs its own test -- reverting only
// this half of the fix left every callback-driven test green.
TEST(CApiBookSnapshot, SymbolContextQueryReportsTheBestLevelSizes)
{
  SymbolRegistry registry;
  SymbolId sym = addSymbol(registry);

  LrvxStrategyCallbacks cb{};
  auto bridge = std::make_unique<BridgeStrategy>(1, std::vector<SymbolId>{sym}, registry, cb);
  auto handle = static_cast<LrvxStrategyHandle>(bridge.get());

  BookUpdatePool pool;
  auto update = makeSnapshot(pool, sym,
                             {{Price::fromDouble(100.00), Quantity::fromDouble(2.5)},
                              {Price::fromDouble(99.99), Quantity::fromDouble(7.0)}},
                             {{Price::fromDouble(100.05), Quantity::fromDouble(1.25)},
                              {Price::fromDouble(100.06), Quantity::fromDouble(9.0)}});
  bridge->onBookUpdate(*update);

  LrvxSymbolContext ctx{};
  lrvx_get_symbol_context(handle, sym, &ctx);

  EXPECT_EQ(ctx.symbol_id, sym);
  EXPECT_EQ(ctx.book.bid_price_raw, Price::fromDouble(100.00).raw());
  EXPECT_EQ(ctx.book.ask_price_raw, Price::fromDouble(100.05).raw());
  EXPECT_EQ(ctx.book.bid_qty_raw, Quantity::fromDouble(2.5).raw())
      << "the size resting at the best bid, queried outside a callback";
  EXPECT_EQ(ctx.book.has_bid, 1);
  EXPECT_EQ(ctx.book.has_ask, 1);
  EXPECT_EQ(ctx.book.ask_qty_raw, Quantity::fromDouble(1.25).raw())
      << "the size resting at the best ask, queried outside a callback";
}

TEST(CApiBookSnapshot, SymbolContextQueryReportsZeroForAnEmptySide)
{
  SymbolRegistry registry;
  SymbolId sym = addSymbol(registry);

  LrvxStrategyCallbacks cb{};
  auto bridge = std::make_unique<BridgeStrategy>(1, std::vector<SymbolId>{sym}, registry, cb);
  auto handle = static_cast<LrvxStrategyHandle>(bridge.get());

  BookUpdatePool pool;
  auto update =
      makeSnapshot(pool, sym, {{Price::fromDouble(100.00), Quantity::fromDouble(3.0)}}, {});
  bridge->onBookUpdate(*update);

  LrvxSymbolContext ctx{};
  lrvx_get_symbol_context(handle, sym, &ctx);

  EXPECT_EQ(ctx.book.bid_qty_raw, Quantity::fromDouble(3.0).raw());
  EXPECT_EQ(ctx.book.ask_price_raw, 0);
  EXPECT_EQ(ctx.book.ask_qty_raw, 0)
      << "0 means the book cannot say, which here is the truth";
  EXPECT_EQ(ctx.book.has_bid, 1) << "the bid side has a best level";
  EXPECT_EQ(ctx.book.has_ask, 0)
      << "the ask side has none; the flag, not the zero price, says so";
}
