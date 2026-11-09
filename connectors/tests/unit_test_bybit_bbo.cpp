/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Offline tests for Bybit's level-1 topic as the touch. Recorded frames go
 * into the connector's public handleMessage seam, no socket involved -- the
 * same technique unit_test_bybit_gap uses.
 *
 * What is pinned:
 *
 *  - The subscription topics follow subscribeBbo: on, a symbol gets
 *    orderbook.1 next to its depth topic; a symbol configured with Top1 and
 *    an option get no second topic.
 *
 *  - An orderbook.1 frame becomes a TOP_OF_BOOK event carrying recvNs,
 *    sourceExchange, the matching-engine timestamp and the instrument, and
 *    moves the cross-venue top between two depth frames.
 *
 *  - Level 1 has its own update id and never enters the depth topic's
 *    continuity check: a top between a depth snapshot and its next delta is
 *    not a gap.
 *
 *  - An empty side is an empty side, both sides empty still publishes, and a
 *    side that fails to parse drops the whole frame.
 *
 *  - With Top1 configured as the depth, orderbook.1 stays a depth snapshot.
 */

#include "lrvx-connectors/bybit/bybit_exchange_connector.h"

#include <lrvx/book/bus/book_update_bus.h>
#include <lrvx/book/bus/trade_bus.h>
#include <lrvx/book/composite_book_matrix.h>
#include <lrvx/book/events/book_update_event.h>
#include <lrvx/common.h>
#include <lrvx/engine/symbol_registry.h>
#include <lrvx/log/atomic_logger.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace lrvx;

namespace
{

std::string tempLogDir()
{
  auto dir = std::filesystem::temp_directory_path() / "lrvx_bybit_bbo_logs";
  std::filesystem::create_directories(dir);
  return dir.string();
}

std::shared_ptr<AtomicLogger> makeLogger(const char* name)
{
  AtomicLoggerOptions logOpts;
  logOpts.directory = tempLogDir();
  logOpts.basename = name;
  return std::make_shared<AtomicLogger>(logOpts);
}

struct SeenBook
{
  uint64_t recvNs;
  ExchangeId sourceExchange;
  BookUpdateType type;
  InstrumentType instrument;
  int64_t exchangeTsNs;
  int64_t seq;
  std::vector<std::pair<int64_t, int64_t>> bids;  // (price raw, qty raw)
  std::vector<std::pair<int64_t, int64_t>> asks;
};

class BookCapture final : public IMarketDataSubscriber
{
 public:
  SubscriberId id() const override { return 73; }

  void onBookUpdate(const BookUpdateEvent& ev) override
  {
    SeenBook seen;
    seen.recvNs = ev.recvNs.raw();
    seen.sourceExchange = ev.sourceExchange;
    seen.type = ev.update.type;
    seen.instrument = ev.update.instrument;
    seen.exchangeTsNs = ev.update.exchangeTsNs.raw();
    seen.seq = ev.seq;
    for (const auto& lvl : ev.update.bids)
    {
      seen.bids.emplace_back(lvl.price.raw(), lvl.quantity.raw());
    }
    for (const auto& lvl : ev.update.asks)
    {
      seen.asks.emplace_back(lvl.price.raw(), lvl.quantity.raw());
    }
    std::lock_guard<std::mutex> lk(_m);
    _books.push_back(std::move(seen));
  }

  std::vector<SeenBook> books()
  {
    std::lock_guard<std::mutex> lk(_m);
    return _books;
  }

 private:
  std::mutex _m;
  std::vector<SeenBook> _books;
};

struct FeedFixture
{
  BookUpdateBus bookBus;
  TradeBus tradeBus;
  SymbolRegistry registry;
  std::shared_ptr<AtomicLogger> logger;
  std::unique_ptr<BybitExchangeConnector> connector;
  BookCapture bookCapture;
  CompositeBookMatrix<4> matrix;

  FeedFixture(const char* logName, BybitConfig::BookDepth depth, bool subscribeBbo)
  {
    logger = makeLogger(logName);
    SymbolInfo btc{};
    btc.symbol = "BTCUSDT";
    btc.exchange = "bybit";
    btc.type = InstrumentType::Future;
    registry.registerSymbol(btc);

    BybitConfig cfg;
    cfg.publicEndpoint = "wss://unused.invalid";
    cfg.symbols = {{"BTCUSDT", InstrumentType::Future, depth}};
    cfg.subscribeBbo = subscribeBbo;
    connector = std::make_unique<BybitExchangeConnector>(cfg, &bookBus, &tradeBus, nullptr,
                                                         &registry, logger);
    matrix.setId(74);
    bookBus.subscribe(&bookCapture);
    bookBus.subscribe(&matrix);
    bookBus.start();
    tradeBus.start();
  }

  ~FeedFixture()
  {
    bookBus.stop();
    tradeBus.stop();
  }

  SymbolId symbol() const
  {
    const auto sym = registry.getSymbolId("bybit", "BTCUSDT");
    return sym ? *sym : 0;
  }
};

// Depth frames on the orderbook.50 topic, as unit_test_bybit_gap builds them.
std::string depthFrame(const char* type, int64_t u, const char* bid, const char* ask)
{
  std::string s = R"({"topic":"orderbook.50.BTCUSDT","type":")";
  s += type;
  s += R"(","ts":1700000000000,"cts":1700000000000,"data":{"s":"BTCUSDT","b":[[")";
  s += bid;
  s += R"(","1"]],"a":[[")";
  s += ask;
  s += R"(","2"]],"u":)";
  s += std::to_string(u);
  s += R"(,"seq":)";
  s += std::to_string(u * 10);
  s += "}}";
  return s;
}

// Level-1 frames: Bybit sends them as type "snapshot" every time, one level
// per side or an empty array for a side with nothing on it.
std::string topFrame(int64_t u, const char* bids, const char* asks, int64_t cts = 1700000001000)
{
  std::string s = R"({"topic":"orderbook.1.BTCUSDT","type":"snapshot","ts":1700000001005,"cts":)";
  s += std::to_string(cts);
  s += R"(,"data":{"s":"BTCUSDT","b":)";
  s += bids;
  s += R"(,"a":)";
  s += asks;
  s += R"(,"u":)";
  s += std::to_string(u);
  s += R"(,"seq":)";
  s += std::to_string(u * 10);
  s += "}}";
  return s;
}

}  // namespace

TEST(BybitBbo, SubscriptionTopicsFollowTheFlag)
{
  BybitConfig cfg;
  EXPECT_FALSE(cfg.subscribeBbo) << "off by default: an event per tick per symbol";
  cfg.symbols = {{"BTCUSDT", InstrumentType::Future, BybitConfig::BookDepth::Top50},
                 {"ETHUSDT", InstrumentType::Future, BybitConfig::BookDepth::Top1},
                 {"BTC-27DEC26-100000-C", InstrumentType::Option, BybitConfig::BookDepth::Top25}};

  const auto off = BybitExchangeConnector::subscriptionTopics(cfg);
  EXPECT_EQ(off, (std::vector<std::string>{"orderbook.50.BTCUSDT", "publicTrade.BTCUSDT",
                                           "orderbook.1.ETHUSDT", "publicTrade.ETHUSDT",
                                           "orderbook.25.BTC-27DEC26-100000-C",
                                           "publicTrade.BTC-27DEC26-100000-C"}));

  cfg.subscribeBbo = true;
  const auto on = BybitExchangeConnector::subscriptionTopics(cfg);
  // BTCUSDT gains the level-1 topic. ETHUSDT already takes its depth from
  // orderbook.1 and gets no second subscription; options have no level 1.
  EXPECT_EQ(on, (std::vector<std::string>{
                    "orderbook.50.BTCUSDT", "publicTrade.BTCUSDT", "orderbook.1.BTCUSDT",
                    "orderbook.1.ETHUSDT", "publicTrade.ETHUSDT",
                    "orderbook.25.BTC-27DEC26-100000-C", "publicTrade.BTC-27DEC26-100000-C"}));
}

TEST(BybitBbo, LevelOneFrameIsTopOfBookWithEveryHeaderField)
{
  FeedFixture f("bybit_bbo_top.log", BybitConfig::BookDepth::Top50, true);
  const ExchangeId expected = f.registry.getExchangeId(f.connector->exchangeId());
  ASSERT_NE(expected, InvalidExchangeId);

  f.connector->handleMessage(topFrame(7, R"([["60005","1.5"]])", R"([["60009","2"]])"));
  f.bookBus.flush();

  const auto books = f.bookCapture.books();
  ASSERT_EQ(books.size(), 1u);
  const auto& ev = books[0];
  EXPECT_EQ(ev.type, BookUpdateType::TOP_OF_BOOK);
  EXPECT_EQ(ev.instrument, InstrumentType::Future);
  ASSERT_EQ(ev.bids.size(), 1u);
  ASSERT_EQ(ev.asks.size(), 1u);
  EXPECT_EQ(ev.bids[0].first, Price::fromDouble(60005.0).raw());
  EXPECT_EQ(ev.bids[0].second, Quantity::fromDouble(1.5).raw());
  EXPECT_EQ(ev.asks[0].first, Price::fromDouble(60009.0).raw());
  EXPECT_EQ(ev.exchangeTsNs, 1700000001000LL * 1'000'000LL) << "cts, the matching-engine stamp";
  EXPECT_EQ(ev.seq, 7);
  EXPECT_NE(ev.recvNs, 0u) << "recvNs gates CompositeBookMatrix::checkStaleness";
  EXPECT_EQ(ev.sourceExchange, expected)
      << "sourceExchange must be the connector's exchange id; InvalidExchangeId makes "
         "CompositeBookMatrix drop the update";
}

TEST(BybitBbo, LevelOneMovesTheTopBetweenDepthFrames)
{
  FeedFixture f("bybit_bbo_matrix.log", BybitConfig::BookDepth::Top50, true);
  f.connector->handleMessage(depthFrame("snapshot", 100, "60000", "60010"));
  f.connector->handleMessage(topFrame(7, R"([["60005","1.5"]])", R"([["60009","2"]])"));
  f.bookBus.flush();

  const SymbolId sym = f.symbol();
  ASSERT_NE(sym, 0u);
  // The depth snapshot put the top at 60000/60010; the level-1 frame that
  // followed is the venue's newer word on it.
  auto bid = f.matrix.bestBid(sym);
  ASSERT_TRUE(bid.valid);
  EXPECT_EQ(bid.priceRaw, Price::fromDouble(60005.0).raw());
  auto ask = f.matrix.bestAsk(sym);
  ASSERT_TRUE(ask.valid);
  EXPECT_EQ(ask.priceRaw, Price::fromDouble(60009.0).raw());
}

TEST(BybitBbo, LevelOneNeverEntersTheDepthContinuityCheck)
{
  FeedFixture f("bybit_bbo_continuity.log", BybitConfig::BookDepth::Top50, true);
  f.connector->handleMessage(depthFrame("snapshot", 100, "60000", "60010"));
  // Level 1 runs its own update id. If the connector fed it into the depth
  // topic's bookkeeping, the delta at u=101 would read as a gap after u=9000.
  f.connector->handleMessage(topFrame(9000, R"([["60005","1.5"]])", R"([["60009","2"]])"));
  f.connector->handleMessage(topFrame(9000, R"([["60005","1.5"]])", R"([["60009","2"]])"));
  f.connector->handleMessage(depthFrame("delta", 101, "60001", "60010"));
  f.bookBus.flush();

  EXPECT_EQ(f.connector->bookGapCount(), 0u);
  const auto books = f.bookCapture.books();
  ASSERT_EQ(books.size(), 4u) << "snapshot, two tops (the repeat included), delta";
  EXPECT_EQ(books[0].type, BookUpdateType::SNAPSHOT);
  EXPECT_EQ(books[1].type, BookUpdateType::TOP_OF_BOOK);
  EXPECT_EQ(books[2].type, BookUpdateType::TOP_OF_BOOK);
  EXPECT_EQ(books[3].type, BookUpdateType::DELTA);
  EXPECT_EQ(books[3].seq, 101);
}

TEST(BybitBbo, EmptySideIsEmptyAndBothEmptyStillPublishes)
{
  FeedFixture f("bybit_bbo_empty.log", BybitConfig::BookDepth::Top50, true);
  f.connector->handleMessage(depthFrame("snapshot", 100, "60000", "60010"));
  f.connector->handleMessage(topFrame(7, "[]", R"([["60009","2"]])"));
  f.connector->handleMessage(topFrame(8, "[]", "[]"));
  f.bookBus.flush();

  const auto books = f.bookCapture.books();
  ASSERT_EQ(books.size(), 3u) << "the one-sided top and the both-empty top both publish";
  EXPECT_TRUE(books[1].bids.empty());
  ASSERT_EQ(books[1].asks.size(), 1u);
  EXPECT_TRUE(books[2].bids.empty());
  EXPECT_TRUE(books[2].asks.empty());
  EXPECT_EQ(books[2].type, BookUpdateType::TOP_OF_BOOK);

  // Both sides empty is the venue saying the touch is gone; the depth
  // snapshot's top from three frames ago must not keep standing.
  const SymbolId sym = f.symbol();
  EXPECT_FALSE(f.matrix.bestBid(sym).valid);
  EXPECT_FALSE(f.matrix.bestAsk(sym).valid);
}

TEST(BybitBbo, InvalidSideDropsTheWholeFrame)
{
  FeedFixture f("bybit_bbo_invalid.log", BybitConfig::BookDepth::Top50, true);
  f.connector->handleMessage(topFrame(7, R"([["not-a-price","1"]])", R"([["60009","2"]])"));
  f.bookBus.flush();
  // A half-published frame would empty the broken side downstream, which
  // is exactly what a genuinely empty side looks like.
  EXPECT_TRUE(f.bookCapture.books().empty());
}

TEST(BybitBbo, LevelOneConfiguredAsDepthStaysASnapshot)
{
  FeedFixture f("bybit_bbo_top1_depth.log", BybitConfig::BookDepth::Top1, true);
  f.connector->handleMessage(topFrame(7, R"([["60005","1.5"]])", R"([["60009","2"]])"));
  f.connector->handleMessage(topFrame(8, "[]", "[]"));
  f.bookBus.flush();

  // The symbol takes its depth from orderbook.1, so the frame is the book,
  // not the touch, and an empty one is dropped as it always was.
  const auto books = f.bookCapture.books();
  ASSERT_EQ(books.size(), 1u);
  EXPECT_EQ(books[0].type, BookUpdateType::SNAPSHOT);
}

TEST(BybitBbo, FlagOffLeavesLevelOneAsASnapshot)
{
  FeedFixture f("bybit_bbo_flag_off.log", BybitConfig::BookDepth::Top50, false);
  f.connector->handleMessage(topFrame(7, R"([["60005","1.5"]])", R"([["60009","2"]])"));
  f.bookBus.flush();

  // Not subscribed, so never expected; a frame that arrives anyway is read
  // as the depth frame it was before the flag existed.
  const auto books = f.bookCapture.books();
  ASSERT_EQ(books.size(), 1u);
  EXPECT_EQ(books[0].type, BookUpdateType::SNAPSHOT);
}
