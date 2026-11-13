/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Polymarket changes a market's tick while the connector is connected and
 * says so with a tick_size_change message on the public market channel. The
 * connector follows it: the registry entry takes the new tick, and a strategy
 * books on it from its next event.
 *
 * The frames under fixtures/polymarket have the shape the venue documents for
 * the market channel (docs.polymarket.com, WebSocket market channel):
 *   {"event_type":"tick_size_change","market":"0x..","asset_id":"..",
 *    "old_tick_size":"0.01","new_tick_size":"0.001","timestamp":"<ms>"}
 * old_tick_size is optional there, and one fixture leaves it out with the
 * fields in another order. Frames are fed straight into handleMessage; the
 * tick-size answers come from fixtures/instrument_info. No sockets, no
 * network.
 */

#include "lrvx-connectors/polymarket/polymarket_exchange_connector.h"

#include <lrvx/book/bus/book_update_bus.h>
#include <lrvx/book/bus/trade_bus.h>
#include <lrvx/book/events/book_update_event.h>
#include <lrvx/common.h>
#include <lrvx/execution/order_price_rule.h>
#include <lrvx/log/atomic_logger.h>
#include <lrvx/strategy/strategy.h>

#include <gtest/gtest.h>

#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <memory_resource>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace lrvx;

namespace
{

const std::string kToken =
    "107505882767731489358349912513945399560393482969656700824895970500493757150417";
const std::string kOtherToken = "OTHER";

constexpr int64_t kCent = 1'000'000;  // 0.01
constexpr int64_t kMill = 100'000;    // 0.001

std::string fixture(const std::string& relative)
{
  std::ifstream in(std::string(LRVX_CONNECTOR_FIXTURES) + "/" + relative);
  EXPECT_TRUE(in.good()) << "missing fixture " << relative;
  std::stringstream ss;
  ss << in.rdbuf();
  std::string body = ss.str();
  while (!body.empty() && (body.back() == '\n' || body.back() == '\r'))
  {
    body.pop_back();
  }
  return body;
}

std::string frame(const std::string& name) { return fixture("polymarket/" + name); }

std::shared_ptr<AtomicLogger> makeLogger(const char* name)
{
  auto dir = std::filesystem::temp_directory_path() / "lrvx_poly_tick_change_logs";
  std::filesystem::create_directories(dir);
  AtomicLoggerOptions opts;
  opts.directory = dir.string();
  opts.basename = name;
  return std::make_shared<AtomicLogger>(opts);
}

struct Buses
{
  BookUpdateBus book;
  TradeBus trade;
  Buses()
  {
    book.start();
    trade.start();
  }
  ~Buses()
  {
    book.stop();
    trade.stop();
  }
};

// The connector as start() leaves it once the venue said the token's tick is
// 0.01: both tokens loaded, nothing connected.
struct Loaded
{
  Buses buses;
  SymbolRegistry registry;
  std::unique_ptr<PolymarketExchangeConnector> connector;

  explicit Loaded(const char* log)
  {
    PolymarketConfig cfg;
    cfg.instrumentInfoUrl = "https://fixture.invalid";
    cfg.instrumentFetch = [](const std::string&,
                             const std::string&) -> std::expected<std::string, std::string>
    {
      return fixture("instrument_info/polymarket_tick_001.json");
    };
    cfg.tokenIds = {kToken, kOtherToken};
    connector = std::make_unique<PolymarketExchangeConnector>(cfg, &buses.book, &buses.trade,
                                                              &registry, makeLogger(log));
  }

  void load() { connector->loadInstrumentSpecs(); }

  SymbolId id(const std::string& token) const { return *registry.getSymbolId("polymarket", token); }
  SymbolInfo info(const std::string& token) const { return *registry.getSymbolInfo(id(token)); }
};

SymbolId preRegister(SymbolRegistry& registry, const std::string& token,
                     std::optional<double> declaredTick = std::nullopt)
{
  SymbolInfo info;
  info.exchange = "polymarket";
  info.symbol = token;
  info.type = InstrumentType::Spot;
  if (declaredTick)
  {
    info.tickSize = Price::fromDouble(*declaredTick);
    info.tickSizeKnown = true;
  }
  return registry.registerSymbol(info);
}

// Records the tick of the book it is handed, the one place a Strategy reads
// a symbol's tick.
class BookReader final : public Strategy
{
 public:
  BookReader(SymbolId sym, const SymbolRegistry& registry) : Strategy(7, sym, registry) {}
  std::optional<Price> seenTick;
  std::optional<Price> seenBestBid;

 protected:
  void onSymbolBook(SymbolContext& c, const BookUpdateEvent&) override
  {
    seenTick = c.book.tickSize();
    seenBestBid = c.book.bestBid();
  }
};

void feedSnapshot(BookReader& strategy, SymbolId sym, std::vector<double> bids, double ask)
{
  BookUpdateEvent ev(std::pmr::get_default_resource());
  ev.update.symbol = sym;
  ev.update.type = BookUpdateType::SNAPSHOT;
  for (double b : bids)
  {
    ev.update.bids.emplace_back(Price::fromDouble(b), Quantity::fromDouble(10));
  }
  ev.update.asks.emplace_back(Price::fromDouble(ask), Quantity::fromDouble(10));
  strategy.onBookUpdate(ev);
}

}  // namespace

TEST(PolymarketTickSizeChange, TheRegistryTakesTheNewTick)
{
  Loaded s("registry.log");
  s.load();
  ASSERT_EQ(s.info(kToken).tickSize.raw(), kCent);
  ASSERT_TRUE(s.connector->instrumentInfoError().empty()) << s.connector->instrumentInfoError();

  s.connector->handleMessage(frame("tick_size_change_001_to_0001.json"));

  EXPECT_TRUE(s.info(kToken).tickSizeKnown);
  EXPECT_EQ(s.info(kToken).tickSize.raw(), kMill) << "0.001 exactly, read from its text";
  EXPECT_EQ(orderTickRaw(s.info(kToken)), kMill);
  ASSERT_TRUE(s.connector->instrumentSpec(kToken));
  EXPECT_EQ(s.connector->instrumentSpec(kToken)->tickSize.raw(), kMill);
  EXPECT_TRUE(s.connector->instrumentInfoError().empty()) << s.connector->instrumentInfoError();

  // Only the market named in the message moves.
  EXPECT_EQ(s.info(kOtherToken).tickSize.raw(), kCent);
  EXPECT_EQ(s.connector->instrumentSpec(kOtherToken)->tickSize.raw(), kCent);
}

TEST(PolymarketTickSizeChange, TheTickMovesBackAsWell)
{
  Loaded s("back.log");
  s.load();
  s.connector->handleMessage(frame("tick_size_change_001_to_0001.json"));
  ASSERT_EQ(s.info(kToken).tickSize.raw(), kMill);

  s.connector->handleMessage(frame("tick_size_change_0001_to_001.json"));
  EXPECT_EQ(s.info(kToken).tickSize.raw(), kCent);
  EXPECT_TRUE(s.connector->instrumentInfoError().empty()) << s.connector->instrumentInfoError();
}

TEST(PolymarketTickSizeChange, FieldOrderAndAMissingOldTickDoNotMatter)
{
  Loaded s("reordered.log");
  s.load();
  s.connector->handleMessage(frame("tick_size_change_reordered_no_old.json"));
  EXPECT_EQ(s.info(kToken).tickSize.raw(), kMill);
}

TEST(PolymarketTickSizeChange, ASymbolRegisteredBeforeStartFollowsToo)
{
  Loaded s("prereg.log");
  const SymbolId pre = preRegister(s.registry, kToken);
  s.load();
  ASSERT_EQ(s.id(kToken), pre);
  ASSERT_EQ(s.info(kToken).tickSize.raw(), kCent);

  s.connector->handleMessage(frame("tick_size_change_001_to_0001.json"));
  EXPECT_EQ(s.info(kToken).tickSize.raw(), kMill);
  EXPECT_TRUE(s.connector->instrumentInfoError().empty()) << s.connector->instrumentInfoError();
}

TEST(PolymarketTickSizeChange, AStrategyBooksOnTheNewTickFromItsNextEvent)
{
  Loaded s("strategy.log");
  const SymbolId sym = preRegister(s.registry, kToken);
  BookReader strategy(sym, s.registry);
  s.load();

  feedSnapshot(strategy, sym, {0.95, 0.94}, 0.97);
  ASSERT_TRUE(strategy.seenTick);
  EXPECT_EQ(strategy.seenTick->raw(), kCent);

  s.connector->handleMessage(frame("tick_size_change_001_to_0001.json"));

  // On the 0.01 grid 0.955 and 0.956 share a bucket; on the new tick the
  // finer best bid is the book's.
  feedSnapshot(strategy, sym, {0.956, 0.955}, 0.958);
  EXPECT_EQ(strategy.seenTick->raw(), kMill);
  ASSERT_TRUE(strategy.seenBestBid);
  EXPECT_EQ(strategy.seenBestBid->raw(), Price::fromDouble(0.956).raw());
}

TEST(PolymarketTickSizeChange, ADeclaredTickIsKeptAndTheConflictReported)
{
  Loaded s("declared.log");
  preRegister(s.registry, kToken, 0.01);
  s.load();
  // Declared 0.01, venue 0.01: nothing to report yet.
  ASSERT_TRUE(s.connector->instrumentInfoError().empty()) << s.connector->instrumentInfoError();

  s.connector->handleMessage(frame("tick_size_change_001_to_0001.json"));

  EXPECT_EQ(s.info(kToken).tickSize.raw(), kCent) << "the declared tick is the caller's";
  const std::string err = s.connector->instrumentInfoError();
  EXPECT_NE(err.find(kToken), std::string::npos) << err;
  EXPECT_NE(err.find("0.001"), std::string::npos) << err;
  EXPECT_NE(err.find("declared tick is kept"), std::string::npos) << err;
  // What the venue uses is still on record for whoever asks.
  EXPECT_EQ(s.connector->instrumentSpec(kToken)->tickSize.raw(), kMill);

  // The same message again says nothing new.
  s.connector->handleMessage(frame("tick_size_change_001_to_0001.json"));
  EXPECT_EQ(s.connector->instrumentInfoError(), err);
}

TEST(PolymarketTickSizeChange, AMessageWithNoUsableTickChangesNothing)
{
  Loaded s("unusable.log");
  s.load();

  s.connector->handleMessage(frame("tick_size_change_zero.json"));
  s.connector->handleMessage(R"({"event_type":"tick_size_change","asset_id":")" + kToken + R"("})");
  s.connector->handleMessage(
      R"({"event_type":"tick_size_change","asset_id":"NOT_OURS","new_tick_size":"0.001"})");

  EXPECT_EQ(s.info(kToken).tickSize.raw(), kCent);
  EXPECT_FALSE(s.registry.getSymbolId("polymarket", "NOT_OURS"))
      << "a market the connector was not configured with is not registered by a tick message";
}

namespace
{
std::string tickChange(const std::string& token, const std::string& tick)
{
  return R"({"event_type":"tick_size_change","asset_id":")" + token + R"(","new_tick_size":")" +
         tick + R"("})";
}

size_t occurrences(const std::string& text, const std::string& needle)
{
  size_t n = 0;
  for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1))
  {
    ++n;
  }
  return n;
}
}  // namespace

// The report was a string that only grew: a token whose tick the venue could
// not be asked for at start() stayed in it after the venue supplied the tick.
TEST(PolymarketTickSizeChange, ATickTheVenueSuppliesLaterClearsTheLoadFailure)
{
  Buses buses;
  SymbolRegistry registry;
  PolymarketConfig cfg;
  cfg.instrumentInfoUrl = "https://fixture.invalid";
  cfg.instrumentFetch = [](const std::string&,
                           const std::string&) -> std::expected<std::string, std::string>
  {
    return std::string(R"({"error":"market not found"})");
  };
  cfg.tokenIds = {kToken, kOtherToken};
  PolymarketExchangeConnector connector(cfg, &buses.book, &buses.trade, &registry,
                                        makeLogger("late.log"));
  connector.loadInstrumentSpecs();

  const std::string atStart = connector.instrumentInfoError();
  ASSERT_NE(atStart.find(kToken), std::string::npos) << atStart;
  ASSERT_NE(atStart.find(kOtherToken), std::string::npos) << atStart;
  ASSERT_FALSE(registry.getSymbolInfo(*registry.getSymbolId("polymarket", kToken))->tickSizeKnown);

  connector.handleMessage(tickChange(kToken, "0.001"));

  const auto info = *registry.getSymbolInfo(*registry.getSymbolId("polymarket", kToken));
  EXPECT_TRUE(info.tickSizeKnown);
  EXPECT_EQ(info.tickSize.raw(), kMill);
  // The token is first known here, so its documented size step arrives with
  // the tick.
  EXPECT_EQ(info.lotSize.raw(), instrument_info::kPolymarketSizeStep.raw());
  const std::string after = connector.instrumentInfoError();
  EXPECT_EQ(after.find(kToken), std::string::npos)
      << "the token's tick is known now, the report still says: " << after;
  EXPECT_NE(after.find(kOtherToken), std::string::npos)
      << "the other token is still unread: " << after;
  EXPECT_NE(after.find("market not found"), std::string::npos) << after;

  connector.handleMessage(tickChange(kOtherToken, "0.01"));
  EXPECT_TRUE(connector.instrumentInfoError().empty()) << connector.instrumentInfoError();
}

// A conflict with a declared tick stays for as long as the two differ, as one
// entry that names the venue's current tick.
TEST(PolymarketTickSizeChange, AConflictIsOneEntryThatFollowsTheVenue)
{
  Loaded s("conflict.log");
  preRegister(s.registry, kToken, 0.01);
  s.load();
  ASSERT_TRUE(s.connector->instrumentInfoError().empty()) << s.connector->instrumentInfoError();

  s.connector->handleMessage(tickChange(kToken, "0.001"));
  const std::string first = s.connector->instrumentInfoError();
  EXPECT_NE(first.find("the venue uses 0.001000;"), std::string::npos) << first;

  s.connector->handleMessage(tickChange(kToken, "0.0001"));
  const std::string second = s.connector->instrumentInfoError();
  EXPECT_NE(second.find("the venue uses 0.000100;"), std::string::npos) << second;
  EXPECT_EQ(second.find("the venue uses 0.001000;"), std::string::npos)
      << "the earlier venue tick is no longer true: " << second;
  EXPECT_EQ(occurrences(second, kToken), 1u) << second;
  EXPECT_EQ(s.info(kToken).tickSize.raw(), kCent) << "the declared tick is still the caller's";

  // The venue comes back to the declared tick: nothing is in conflict.
  s.connector->handleMessage(tickChange(kToken, "0.01"));
  EXPECT_TRUE(s.connector->instrumentInfoError().empty()) << s.connector->instrumentInfoError();
}

TEST(InstrumentSpecTableReport, KeepsOneEntryPerSymbolInFirstSeenOrder)
{
  InstrumentSpecTable table;
  table.addError("meta: unreachable");
  table.addError("AAA", "HTTP 503");
  table.setConflict("BBB", "the registry declares tick 0.01, the venue uses 0.1");
  table.addError("CCC", "not listed");
  EXPECT_EQ(table.error(),
            "meta: unreachable; AAA: HTTP 503; BBB: the registry declares tick 0.01, the venue "
            "uses 0.1; CCC: not listed");

  table.addError("AAA", "not requested");
  EXPECT_EQ(occurrences(table.error(), "AAA"), 1u);
  EXPECT_NE(table.error().find("AAA: not requested"), std::string::npos);

  InstrumentSpec spec;
  spec.tickSize = Price::fromRaw(kCent);
  table.add("AAA", spec);
  table.add("BBB", spec);
  EXPECT_EQ(table.error(),
            "meta: unreachable; BBB: the registry declares tick 0.01, the venue uses 0.1; CCC: "
            "not listed")
      << "a spec clears the load failure; a conflict is the registry's to clear";

  table.clearConflict("BBB");
  table.clear();
  EXPECT_TRUE(table.error().empty());
}
