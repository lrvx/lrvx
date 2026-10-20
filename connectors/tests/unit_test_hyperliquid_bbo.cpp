/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Offline tests for the Hyperliquid bbo channel. Recorded frames go into the
 * connector's public handleMessage seam, no socket involved — the same
 * technique unit_test_hyperliquid_fill_contract uses.
 *
 * What is pinned:
 *
 *  - The subscription payloads follow the subscribeBbo flag, so turning the
 *    flag off (or deleting the subscribe block) is visible to a test.
 *
 *  - A bbo frame becomes a TOP_OF_BOOK event carrying recvNs, sourceExchange,
 *    the venue timestamp and the instrument — every field a recycled pool
 *    slot would otherwise inherit from its previous occupant.
 *
 *  - Between two l2Book snapshots a bbo frame moves the cross-venue top, and
 *    a null side (an empty book side) zeroes that side in the matrix instead
 *    of leaving a stale quote standing.
 *
 *  - A present-but-unparseable side drops the whole frame. Publishing the
 *    other side would zero this one downstream, indistinguishable from a
 *    genuinely empty book side.
 */

#include "lrvx-connectors/hyperliquid/hyperliquid_exchange_connector.h"

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
  auto dir = std::filesystem::temp_directory_path() / "lrvx_hl_bbo_logs";
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
  std::vector<std::pair<int64_t, int64_t>> bids;  // (price raw, qty raw)
  std::vector<std::pair<int64_t, int64_t>> asks;
};

class BookCapture final : public IMarketDataSubscriber
{
 public:
  SubscriberId id() const override { return 71; }

  void onBookUpdate(const BookUpdateEvent& ev) override
  {
    SeenBook seen;
    seen.recvNs = ev.recvNs.raw();
    seen.sourceExchange = ev.sourceExchange;
    seen.type = ev.update.type;
    seen.instrument = ev.update.instrument;
    seen.exchangeTsNs = ev.update.exchangeTsNs.raw();
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
  std::unique_ptr<HyperliquidExchangeConnector> connector;
  BookCapture bookCapture;
  CompositeBookMatrix<4> matrix;

  explicit FeedFixture(const char* logName)
  {
    logger = makeLogger(logName);
    registry.registerExchange("hyperliquid");

    HyperliquidConfig cfg;
    cfg.wsEndpoint = "wss://unused.invalid";
    cfg.symbols = {"BTC"};
    connector =
        std::make_unique<HyperliquidExchangeConnector>(cfg, &bookBus, &tradeBus, &registry, logger);

    matrix.setId(72);
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
};

constexpr const char* kL2BookFrame =
    R"({"channel":"l2Book","data":{"coin":"BTC","time":1700000000000,"levels":)"
    R"([[{"px":"60000","sz":"3","n":1}],[{"px":"60010","sz":"2","n":1}]]}})";

constexpr const char* kL2BookInvalidFrame =
    R"({"channel":"l2Book","data":{"coin":"BTC","time":1700000000000,"levels":)"
    R"([[{"px":"garbage","sz":"3","n":1}],[{"px":"60010","sz":"2","n":1}]]}})";

constexpr const char* kBboFrame =
    R"({"channel":"bbo","data":{"coin":"BTC","time":1700000001000,"bbo":)"
    R"([{"px":"60005","sz":"1.5","n":3},{"px":"60009","sz":"2","n":1}]}})";

constexpr const char* kBboNoTimeFrame =
    R"({"channel":"bbo","data":{"coin":"BTC","bbo":)"
    R"([{"px":"60006","sz":"1","n":1},{"px":"60008","sz":"1","n":1}]}})";

constexpr const char* kBboAskOnlyFrame =
    R"({"channel":"bbo","data":{"coin":"BTC","time":1700000002000,"bbo":)"
    R"([null,{"px":"60009","sz":"2","n":1}]}})";

constexpr const char* kBboBothNullFrame =
    R"({"channel":"bbo","data":{"coin":"BTC","time":1700000004000,"bbo":[null,null]}})";

constexpr const char* kBboInvalidPxFrame =
    R"({"channel":"bbo","data":{"coin":"BTC","time":1700000003000,"bbo":)"
    R"([{"px":"not-a-price","sz":"1","n":1},{"px":"60009","sz":"2","n":1}]}})";

}  // namespace

TEST(HyperliquidBbo, SubscriptionPayloadsFollowTheFlag)
{
  HyperliquidConfig cfg;
  EXPECT_TRUE(cfg.subscribeBbo);

  auto on = HyperliquidExchangeConnector::subscriptionPayloads(cfg, "BTC");
  ASSERT_EQ(on.size(), 3u);
  EXPECT_EQ(on[0], R"({"method":"subscribe","subscription":{"type":"l2Book","coin":"BTC"}})");
  EXPECT_EQ(on[1], R"({"method":"subscribe","subscription":{"type":"trades","coin":"BTC"}})");
  EXPECT_EQ(on[2], R"({"method":"subscribe","subscription":{"type":"bbo","coin":"BTC"}})");

  cfg.subscribeBbo = false;
  auto off = HyperliquidExchangeConnector::subscriptionPayloads(cfg, "BTC");
  ASSERT_EQ(off.size(), 2u);
  EXPECT_EQ(off[0], on[0]);
  EXPECT_EQ(off[1], on[1]);
}

TEST(HyperliquidBbo, BboFrameIsTopOfBookWithEveryHeaderField)
{
  FeedFixture f("hl_bbo_snapshot.log");
  const ExchangeId expected = f.registry.getExchangeId(f.connector->exchangeId());
  ASSERT_NE(expected, InvalidExchangeId);

  f.connector->handleMessage(kBboFrame);
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
  EXPECT_NE(ev.recvNs, 0u) << "recvNs gates CompositeBookMatrix::checkStaleness; at zero the "
                              "venue is skipped and a frozen feed stays quotable";
  EXPECT_EQ(ev.sourceExchange, expected)
      << "sourceExchange must be the connector's exchange id; InvalidExchangeId makes "
         "CompositeBookMatrix drop the update";
}

TEST(HyperliquidBbo, RecycledSlotDoesNotInheritTheExchangeTimestamp)
{
  FeedFixture f("hl_bbo_recycled_slot.log");

  // First frame stamps the slot with a venue time; the second carries none.
  // The pool hands slots back LIFO, so without an unconditional write the
  // second event would surface the first frame's timestamp.
  f.connector->handleMessage(kBboFrame);
  f.bookBus.flush();
  f.connector->handleMessage(kBboNoTimeFrame);
  f.bookBus.flush();

  const auto books = f.bookCapture.books();
  ASSERT_EQ(books.size(), 2u);
  EXPECT_EQ(books[0].exchangeTsNs, 1700000001000LL * 1'000'000LL);
  EXPECT_EQ(books[1].exchangeTsNs, 0);
}

TEST(HyperliquidBbo, BboMovesTheTopBetweenL2Snapshots)
{
  FeedFixture f("hl_bbo_refresh.log");

  f.connector->handleMessage(kL2BookFrame);
  f.connector->handleMessage(kBboFrame);
  f.bookBus.flush();

  const auto sym = f.registry.getSymbolId("hyperliquid", "BTC");
  ASSERT_TRUE(sym.has_value());

  // The l2Book snapshot put the top at 60000/60010; the bbo frame that
  // followed is the venue's newer word on it.
  auto bid = f.matrix.bestBid(*sym);
  ASSERT_TRUE(bid.valid);
  EXPECT_EQ(bid.priceRaw, Price::fromDouble(60005.0).raw());

  auto ask = f.matrix.bestAsk(*sym);
  ASSERT_TRUE(ask.valid);
  EXPECT_EQ(ask.priceRaw, Price::fromDouble(60009.0).raw());
}

TEST(HyperliquidBbo, NullSideZeroesTheMatrixSide)
{
  FeedFixture f("hl_bbo_null_side.log");

  f.connector->handleMessage(kL2BookFrame);
  f.connector->handleMessage(kBboAskOnlyFrame);
  f.bookBus.flush();

  const auto books = f.bookCapture.books();
  ASSERT_EQ(books.size(), 2u);
  EXPECT_TRUE(books[1].bids.empty());
  ASSERT_EQ(books[1].asks.size(), 1u);

  // The bbo frame is the venue's complete word on the touch: no bid side
  // means no bids, and the l2Book bid from two frames ago must not keep
  // standing in the cross-venue comparison.
  const auto sym = f.registry.getSymbolId("hyperliquid", "BTC");
  ASSERT_TRUE(sym.has_value());
  EXPECT_FALSE(f.matrix.bestBid(*sym).valid);
  auto ask = f.matrix.bestAsk(*sym);
  ASSERT_TRUE(ask.valid);
  EXPECT_EQ(ask.priceRaw, Price::fromDouble(60009.0).raw());
}

TEST(HyperliquidBbo, BothSidesNullStillPublishesAndZeroesTheMatrix)
{
  FeedFixture f("hl_bbo_both_null.log");

  f.connector->handleMessage(kL2BookFrame);
  f.connector->handleMessage(kBboBothNullFrame);
  f.bookBus.flush();

  // Both sides null is the venue saying the touch is gone. Dropping the
  // frame as "empty" would leave the l2Book top standing in the matrix.
  const auto books = f.bookCapture.books();
  ASSERT_EQ(books.size(), 2u);
  EXPECT_TRUE(books[1].bids.empty());
  EXPECT_TRUE(books[1].asks.empty());
  EXPECT_EQ(books[1].type, BookUpdateType::TOP_OF_BOOK);

  const auto sym = f.registry.getSymbolId("hyperliquid", "BTC");
  ASSERT_TRUE(sym.has_value());
  EXPECT_FALSE(f.matrix.bestBid(*sym).valid);
  EXPECT_FALSE(f.matrix.bestAsk(*sym).valid);
}

TEST(HyperliquidBbo, InvalidSideDropsTheWholeFrame)
{
  FeedFixture f("hl_bbo_invalid.log");

  f.connector->handleMessage(kBboInvalidPxFrame);
  f.connector->handleMessage(kL2BookInvalidFrame);
  f.bookBus.flush();

  // Neither frame may publish: a half-published frame would zero the broken
  // side downstream, which is exactly what a genuine null side looks like.
  EXPECT_TRUE(f.bookCapture.books().empty());
}
