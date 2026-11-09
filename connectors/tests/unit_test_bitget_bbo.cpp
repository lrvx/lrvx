/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Offline tests for Bitget's books1 channel as the touch. Recorded frames go
 * into the connector's public handleMessage seam, no socket involved -- the
 * same technique unit_test_bitget_book_integrity uses.
 *
 * What is pinned:
 *
 *  - The subscribe args follow subscribeBbo: on, a symbol gets books1 next
 *    to its depth channel; a symbol configured with Depth1 gets no second
 *    subscription.
 *
 *  - A books1 frame becomes a TOP_OF_BOOK event carrying recvNs,
 *    sourceExchange, the venue timestamp and the instrument, and moves the
 *    cross-venue top between two depth frames.
 *
 *  - books1 runs its own seq and never enters the depth channel's integrity
 *    check: a top between a depth snapshot and its next update is not a gap.
 *
 *  - An empty side is an empty side, both sides empty still publishes, and a
 *    side that fails to parse drops the whole frame.
 *
 *  - With Depth1 configured as the depth, books1 stays a depth snapshot.
 */

#include "lrvx-connectors/bitget/bitget_exchange_connector.h"

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
  auto dir = std::filesystem::temp_directory_path() / "lrvx_bitget_bbo_logs";
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
  SubscriberId id() const override { return 75; }

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
  std::unique_ptr<BitgetExchangeConnector> connector;
  BookCapture bookCapture;
  CompositeBookMatrix<4> matrix;

  FeedFixture(const char* logName, BitgetConfig::BookDepth depth, bool subscribeBbo)
  {
    logger = makeLogger(logName);
    SymbolInfo btc{};
    btc.symbol = "BTCUSDT";
    btc.exchange = "bitget";
    btc.type = InstrumentType::Future;
    registry.registerSymbol(btc);

    BitgetConfig cfg;
    cfg.publicEndpoint = "wss://unused.invalid";
    cfg.symbols = {{"BTCUSDT", InstrumentType::Future, depth}};
    cfg.subscribeBbo = subscribeBbo;
    connector = std::make_unique<BitgetExchangeConnector>(cfg, &bookBus, &tradeBus, nullptr,
                                                          &registry, logger);
    matrix.setId(76);
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
    const auto sym = registry.getSymbolId("bitget", "BTCUSDT");
    return sym ? *sym : 0;
  }
};

// Frames in the shape unit_test_bitget_book_integrity builds: the depth
// channel carries seq and, on the depth-limited channels, checksum 0.
std::string frame(const char* channel, const char* action, int64_t seq, const char* bids,
                  const char* asks, const char* ts = "1700000001000")
{
  std::string s = R"({"action":")";
  s += action;
  s += R"(","arg":{"instType":"USDT-FUTURES","channel":")";
  s += channel;
  s += R"(","instId":"BTCUSDT"},"data":[{"asks":)";
  s += asks;
  s += R"(,"bids":)";
  s += bids;
  s += R"(,"checksum":0,"seq":)";
  s += std::to_string(seq);
  s += R"(,"ts":")";
  s += ts;
  s += R"("}],"ts":1700000001005})";
  return s;
}

std::string depthFrame(const char* action, int64_t seq, const char* bid, const char* ask)
{
  std::string bids = R"([[")";
  bids += bid;
  bids += R"(","1"]])";
  std::string asks = R"([[")";
  asks += ask;
  asks += R"(","2"]])";
  return frame("books15", action, seq, bids.c_str(), asks.c_str());
}

std::string topFrame(int64_t seq, const char* bids, const char* asks)
{
  return frame("books1", "snapshot", seq, bids, asks);
}

}  // namespace

TEST(BitgetBbo, SubscriptionArgsFollowTheFlag)
{
  BitgetConfig cfg;
  EXPECT_FALSE(cfg.subscribeBbo) << "off by default: an event per tick per symbol";
  cfg.symbols = {{"BTCUSDT", InstrumentType::Future, BitgetConfig::BookDepth::Depth15},
                 {"ETHUSDT", InstrumentType::Future, BitgetConfig::BookDepth::Depth1}};

  const auto off = BitgetExchangeConnector::subscriptionArgs(cfg);
  EXPECT_EQ(off, (std::vector<std::string>{
                     R"({"instType":"USDT-FUTURES","channel":"books15","instId":"BTCUSDT"})",
                     R"({"instType":"USDT-FUTURES","channel":"trade","instId":"BTCUSDT"})",
                     R"({"instType":"USDT-FUTURES","channel":"books1","instId":"ETHUSDT"})",
                     R"({"instType":"USDT-FUTURES","channel":"trade","instId":"ETHUSDT"})"}));

  cfg.subscribeBbo = true;
  const auto on = BitgetExchangeConnector::subscriptionArgs(cfg);
  // BTCUSDT gains books1. ETHUSDT already takes its depth from books1 and
  // gets no second subscription.
  EXPECT_EQ(on, (std::vector<std::string>{
                    R"({"instType":"USDT-FUTURES","channel":"books15","instId":"BTCUSDT"})",
                    R"({"instType":"USDT-FUTURES","channel":"trade","instId":"BTCUSDT"})",
                    R"({"instType":"USDT-FUTURES","channel":"books1","instId":"BTCUSDT"})",
                    R"({"instType":"USDT-FUTURES","channel":"books1","instId":"ETHUSDT"})",
                    R"({"instType":"USDT-FUTURES","channel":"trade","instId":"ETHUSDT"})"}));
}

TEST(BitgetBbo, Books1FrameIsTopOfBookWithEveryHeaderField)
{
  FeedFixture f("bitget_bbo_top.log", BitgetConfig::BookDepth::Depth15, true);
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
  EXPECT_EQ(ev.exchangeTsNs, 1700000001000LL * 1'000'000LL);
  EXPECT_EQ(ev.seq, 7);
  EXPECT_NE(ev.recvNs, 0u) << "recvNs gates CompositeBookMatrix::checkStaleness";
  EXPECT_EQ(ev.sourceExchange, expected)
      << "sourceExchange must be the connector's exchange id; InvalidExchangeId makes "
         "CompositeBookMatrix drop the update";
}

TEST(BitgetBbo, Books1MovesTheTopBetweenDepthFrames)
{
  FeedFixture f("bitget_bbo_matrix.log", BitgetConfig::BookDepth::Depth15, true);
  f.connector->handleMessage(depthFrame("snapshot", 100, "60000", "60010"));
  f.connector->handleMessage(topFrame(7, R"([["60005","1.5"]])", R"([["60009","2"]])"));
  f.bookBus.flush();

  const SymbolId sym = f.symbol();
  ASSERT_NE(sym, 0u);
  auto bid = f.matrix.bestBid(sym);
  ASSERT_TRUE(bid.valid);
  EXPECT_EQ(bid.priceRaw, Price::fromDouble(60005.0).raw());
  auto ask = f.matrix.bestAsk(sym);
  ASSERT_TRUE(ask.valid);
  EXPECT_EQ(ask.priceRaw, Price::fromDouble(60009.0).raw());
}

TEST(BitgetBbo, Books1NeverEntersTheDepthIntegrityCheck)
{
  FeedFixture f("bitget_bbo_integrity.log", BitgetConfig::BookDepth::Depth15, true);
  f.connector->handleMessage(depthFrame("snapshot", 100, "60000", "60010"));
  // books1 runs its own seq. If the connector fed it into the depth
  // channel's bookkeeping, the update at seq 101 would read as a gap after
  // 9000 and be dropped with a resync.
  f.connector->handleMessage(topFrame(9000, R"([["60005","1.5"]])", R"([["60009","2"]])"));
  f.connector->handleMessage(depthFrame("update", 101, "60001", "60010"));
  f.bookBus.flush();

  EXPECT_EQ(f.connector->bookGapCount(), 0u);
  EXPECT_EQ(f.connector->bookChecksumFailureCount(), 0u);
  const auto books = f.bookCapture.books();
  ASSERT_EQ(books.size(), 3u) << "snapshot, top, update";
  EXPECT_EQ(books[0].type, BookUpdateType::SNAPSHOT);
  EXPECT_EQ(books[1].type, BookUpdateType::TOP_OF_BOOK);
  EXPECT_EQ(books[2].type, BookUpdateType::DELTA);
  EXPECT_EQ(books[2].seq, 101);
}

TEST(BitgetBbo, EmptySideIsEmptyAndBothEmptyStillPublishes)
{
  FeedFixture f("bitget_bbo_empty.log", BitgetConfig::BookDepth::Depth15, true);
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

  const SymbolId sym = f.symbol();
  EXPECT_FALSE(f.matrix.bestBid(sym).valid);
  EXPECT_FALSE(f.matrix.bestAsk(sym).valid);
}

TEST(BitgetBbo, InvalidSideDropsTheWholeFrame)
{
  FeedFixture f("bitget_bbo_invalid.log", BitgetConfig::BookDepth::Depth15, true);
  f.connector->handleMessage(topFrame(7, R"([["not-a-price","1"]])", R"([["60009","2"]])"));
  f.bookBus.flush();
  EXPECT_TRUE(f.bookCapture.books().empty());
}

TEST(BitgetBbo, Books1ConfiguredAsDepthStaysASnapshot)
{
  FeedFixture f("bitget_bbo_depth1.log", BitgetConfig::BookDepth::Depth1, true);
  f.connector->handleMessage(topFrame(7, R"([["60005","1.5"]])", R"([["60009","2"]])"));
  f.connector->handleMessage(topFrame(8, "[]", "[]"));
  f.bookBus.flush();

  // The symbol takes its depth from books1, so the frame is the book, not
  // the touch, and an empty one is dropped as it always was.
  const auto books = f.bookCapture.books();
  ASSERT_EQ(books.size(), 1u);
  EXPECT_EQ(books[0].type, BookUpdateType::SNAPSHOT);
}

TEST(BitgetBbo, FlagOffLeavesBooks1AsASnapshot)
{
  FeedFixture f("bitget_bbo_flag_off.log", BitgetConfig::BookDepth::Depth15, false);
  f.connector->handleMessage(topFrame(7, R"([["60005","1.5"]])", R"([["60009","2"]])"));
  f.bookBus.flush();

  const auto books = f.bookCapture.books();
  ASSERT_EQ(books.size(), 1u);
  EXPECT_EQ(books[0].type, BookUpdateType::SNAPSHOT);
}
