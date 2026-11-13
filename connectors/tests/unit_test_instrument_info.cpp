/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Instrument info from each venue's public endpoint reaches the registry, so
 * a live-registered symbol carries the tick the venue enforces and the
 * executor applies the simulator's tick rule to it.
 *
 * The response bodies under fixtures/instrument_info are recorded from the
 * venues' public, unauthenticated endpoints:
 *   https://api.bybit.com/v5/market/instruments-info?category=linear&symbol=BTCUSDT
 *     (also category=spot&symbol=BTCUSDT, category=linear&symbol=1000PEPEUSDT,
 *      and symbol=NOPE for the refusal)
 *   https://api.bitget.com/api/v2/mix/market/contracts?productType=USDT-FUTURES&symbol=BTCUSDT
 *     (also symbol=DOGEUSDT, symbol=NOPEUSDT for the refusal)
 *   https://api.bitget.com/api/v2/spot/public/symbols?symbol=BTCUSDT
 *   POST https://api.hyperliquid.xyz/info {"type":"meta"}
 *     (universe cut to its first 13 coins, marginTables dropped)
 *   https://clob.polymarket.com/tick-size?token_id=<id>
 *     (a 0.001 market, a 0.01 market, and token_id=123 for the refusal)
 * The tests themselves make no network call: every request goes through an
 * injected fetch that answers with those bodies.
 */

#include "loopback_ws_server.h"
#include "lrvx-connectors/bitget/authenticated_rest_client.h"
#include "lrvx-connectors/bitget/bitget_exchange_connector.h"
#include "lrvx-connectors/bitget/bitget_order_executor.h"
#include "lrvx-connectors/bybit/authenticated_rest_client.h"
#include "lrvx-connectors/bybit/bybit_exchange_connector.h"
#include "lrvx-connectors/bybit/bybit_order_executor.h"
#include "lrvx-connectors/execution/instrument_spec.h"
#include "lrvx-connectors/hyperliquid/hyperliquid_exchange_connector.h"
#include "lrvx-connectors/polymarket/polymarket_exchange_connector.h"

#include <lrvx/book/bus/book_update_bus.h>
#include <lrvx/book/bus/trade_bus.h>
#include <lrvx/common.h>
#include <lrvx/execution/abstract_execution_listener.h>
#include <lrvx/execution/bus/order_execution_bus.h>
#include <lrvx/execution/events/order_event.h>
#include <lrvx/execution/order_price_rule.h>
#include <lrvx/execution/order_tracker.h>
#include <lrvx/log/atomic_logger.h>
#include <lrvx/strategy/strategy.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace lrvx;

namespace
{

std::string fixture(const std::string& name)
{
  std::ifstream in(std::string(LRVX_CONNECTOR_FIXTURES) + "/instrument_info/" + name);
  EXPECT_TRUE(in.good()) << "missing fixture " << name;
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::shared_ptr<AtomicLogger> makeLogger(const char* name)
{
  auto dir = std::filesystem::temp_directory_path() / "lrvx_instrument_info_logs";
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

// Answers each URL with a fixture, records what was asked.
struct FixtureFetch
{
  struct Request
  {
    std::string url;
    std::string body;
  };
  std::vector<Request> requests;
  std::vector<std::pair<std::string, std::expected<std::string, std::string>>> answers;

  InstrumentFetch fn()
  {
    return [this](const std::string& url,
                  const std::string& body) -> std::expected<std::string, std::string>
    {
      requests.push_back({url, body});
      for (const auto& [needle, answer] : answers)
      {
        if (url.find(needle) != std::string::npos)
        {
          return answer;
        }
      }
      return std::unexpected<std::string>("no fixture for " + url);
    };
  }
};

}  // namespace

// ------------------------------------------------------------------ parsing

TEST(InstrumentInfoParse, BybitLinearTickAndQtyStep)
{
  auto specs = instrument_info::parseBybit(fixture("bybit_linear_btc.json"));
  ASSERT_TRUE(specs) << specs.error();
  ASSERT_TRUE(specs->contains("BTCUSDT"));
  EXPECT_EQ(specs->at("BTCUSDT").tickSize.raw(), Price::fromDouble(0.1).raw());
  EXPECT_EQ(specs->at("BTCUSDT").lotSize.raw(), Quantity::fromDouble(0.001).raw());
}

TEST(InstrumentInfoParse, BybitFineTickIsExact)
{
  auto specs = instrument_info::parseBybit(fixture("bybit_linear_pepe.json"));
  ASSERT_TRUE(specs) << specs.error();
  // "0.0000010": one millionth, read from the text, not a double.
  EXPECT_EQ(specs->at("1000PEPEUSDT").tickSize.raw(), 100);
  EXPECT_EQ(specs->at("1000PEPEUSDT").lotSize.raw(), Quantity::fromDouble(100).raw());
}

TEST(InstrumentInfoParse, BybitSpotUsesBasePrecisionForTheLot)
{
  auto specs = instrument_info::parseBybit(fixture("bybit_spot_btc.json"));
  ASSERT_TRUE(specs) << specs.error();
  EXPECT_EQ(specs->at("BTCUSDT").tickSize.raw(), Price::fromDouble(0.1).raw());
  EXPECT_EQ(specs->at("BTCUSDT").lotSize.raw(), 100);
}

TEST(InstrumentInfoParse, BybitRefusalCarriesTheVenueMessage)
{
  auto specs = instrument_info::parseBybit(fixture("bybit_symbol_invalid.json"));
  ASSERT_FALSE(specs);
  EXPECT_NE(specs.error().find("10001"), std::string::npos) << specs.error();
  EXPECT_NE(specs.error().find("symbol invalid"), std::string::npos) << specs.error();
}

TEST(InstrumentInfoParse, BitgetContractTickIsPriceEndStepTimesPricePlace)
{
  auto btc = instrument_info::parseBitgetContracts(fixture("bitget_btc.json"));
  ASSERT_TRUE(btc) << btc.error();
  EXPECT_EQ(btc->at("BTCUSDT").tickSize.raw(), Price::fromDouble(0.1).raw());
  EXPECT_EQ(btc->at("BTCUSDT").lotSize.raw(), Quantity::fromDouble(0.0001).raw());

  auto doge = instrument_info::parseBitgetContracts(fixture("bitget_doge.json"));
  ASSERT_TRUE(doge) << doge.error();
  EXPECT_EQ(doge->at("DOGEUSDT").tickSize.raw(), 1000);  // 0.00001
  EXPECT_EQ(doge->at("DOGEUSDT").lotSize.raw(), Quantity::fromDouble(1).raw());
}

TEST(InstrumentInfoParse, BitgetSpotPrecisions)
{
  auto specs = instrument_info::parseBitgetSpot(fixture("bitget_spot_btc.json"));
  ASSERT_TRUE(specs) << specs.error();
  EXPECT_EQ(specs->at("BTCUSDT").tickSize.raw(), Price::fromDouble(0.01).raw());
  EXPECT_EQ(specs->at("BTCUSDT").lotSize.raw(), 100);  // 6 decimals
}

TEST(InstrumentInfoParse, BitgetRefusalCarriesTheVenueMessage)
{
  auto specs = instrument_info::parseBitgetContracts(fixture("bitget_symbol_missing.json"));
  ASSERT_FALSE(specs);
  EXPECT_NE(specs.error().find("40034"), std::string::npos) << specs.error();
  EXPECT_NE(specs.error().find("does not exist"), std::string::npos) << specs.error();
}

TEST(InstrumentInfoParse, HyperliquidTickFollowsSzDecimals)
{
  auto specs = instrument_info::parseHyperliquidMeta(fixture("hyperliquid_meta.json"));
  ASSERT_TRUE(specs) << specs.error();
  EXPECT_EQ(specs->at("BTC").tickSize.raw(), Price::fromDouble(0.1).raw());  // 6 - 5
  EXPECT_EQ(specs->at("BTC").lotSize.raw(), Quantity::fromDouble(0.00001).raw());
  EXPECT_EQ(specs->at("ETH").tickSize.raw(), Price::fromDouble(0.01).raw());  // 6 - 4
  EXPECT_EQ(specs->at("DOGE").tickSize.raw(), 100);                           // 6 - 0
  EXPECT_EQ(specs->at("DOGE").lotSize.raw(), Quantity::fromDouble(1).raw());
}

TEST(InstrumentInfoParse, PolymarketTickIsReadFromItsText)
{
  auto fine = instrument_info::parsePolymarketTickSize(fixture("polymarket_tick_0001.json"));
  ASSERT_TRUE(fine) << fine.error();
  EXPECT_EQ(fine->tickSize.raw(), 100000);  // exactly 0.001
  auto coarse = instrument_info::parsePolymarketTickSize(fixture("polymarket_tick_001.json"));
  ASSERT_TRUE(coarse) << coarse.error();
  EXPECT_EQ(coarse->tickSize.raw(), 1000000);
  // Two size decimals at every tick size, per the CLOB documentation.
  EXPECT_EQ(instrument_info::kPolymarketSizeStep.raw(), Quantity::fromDouble(0.01).raw());
  EXPECT_EQ(fine->lotSize.raw(), Quantity::fromDouble(0.01).raw());
  EXPECT_EQ(coarse->lotSize.raw(), Quantity::fromDouble(0.01).raw());
  auto missing =
      instrument_info::parsePolymarketTickSize(fixture("polymarket_tick_not_found.json"));
  ASSERT_FALSE(missing);
  EXPECT_NE(missing.error().find("market not found"), std::string::npos) << missing.error();
}

// ------------------------------------------------------------------ connector load

TEST(InstrumentInfoLoad, BybitRegistersTheVenueTick)
{
  Buses buses;
  SymbolRegistry registry;
  FixtureFetch fetch;
  fetch.answers.push_back({"symbol=BTCUSDT", fixture("bybit_linear_btc.json")});

  BybitConfig cfg;
  cfg.publicEndpoint = "wss://unused.invalid";
  cfg.instrumentInfoUrl = "https://fixture.invalid";
  cfg.instrumentFetch = fetch.fn();
  cfg.symbols = {{"BTCUSDT", InstrumentType::Future, BybitConfig::BookDepth::Top50}};
  BybitExchangeConnector connector(cfg, &buses.book, &buses.trade, nullptr, &registry,
                                   makeLogger("bybit_load.log"));
  connector.loadInstrumentSpecs();

  ASSERT_EQ(fetch.requests.size(), 1u);
  EXPECT_EQ(fetch.requests[0].url,
            "https://fixture.invalid/v5/market/instruments-info?category=linear&symbol=BTCUSDT");
  EXPECT_TRUE(fetch.requests[0].body.empty()) << "instruments-info is a GET";
  EXPECT_TRUE(connector.instrumentInfoError().empty()) << connector.instrumentInfoError();

  auto id = registry.getSymbolId("bybit", "BTCUSDT");
  ASSERT_TRUE(id);
  auto info = registry.getSymbolInfo(*id);
  ASSERT_TRUE(info);
  EXPECT_TRUE(info->tickSizeKnown);
  EXPECT_EQ(info->tickSize.raw(), Price::fromDouble(0.1).raw());
  EXPECT_EQ(orderTickRaw(*info), Price::fromDouble(0.1).raw());
  EXPECT_EQ(connector.instrumentSpec("BTCUSDT")->lotSize.raw(), Quantity::fromDouble(0.001).raw());
  EXPECT_EQ(info->lotSize.raw(), Quantity::fromDouble(0.001).raw())
      << "the venue's qtyStep reaches the registry entry";
  EXPECT_EQ(orderLotRaw(*info), Quantity::fromDouble(0.001).raw());
}

TEST(InstrumentInfoLoad, BitgetReadsFuturesAndSpotFromTheirOwnEndpoints)
{
  Buses buses;
  SymbolRegistry registry;
  FixtureFetch fetch;
  fetch.answers.push_back({"/api/v2/spot/public/symbols", fixture("bitget_spot_btc.json")});
  fetch.answers.push_back({"/api/v2/mix/market/contracts", fixture("bitget_doge.json")});

  BitgetConfig cfg;
  cfg.publicEndpoint = "wss://unused.invalid";
  cfg.instrumentInfoUrl = "https://fixture.invalid";
  cfg.instrumentFetch = fetch.fn();
  cfg.symbols = {{"DOGEUSDT", InstrumentType::Future, BitgetConfig::BookDepth::Depth15},
                 {"BTCUSDT", InstrumentType::Spot, BitgetConfig::BookDepth::Depth15}};
  BitgetExchangeConnector connector(cfg, &buses.book, &buses.trade, nullptr, &registry,
                                    makeLogger("bitget_load.log"));
  connector.loadInstrumentSpecs();

  ASSERT_EQ(fetch.requests.size(), 2u);
  EXPECT_EQ(fetch.requests[0].url,
            "https://fixture.invalid/api/v2/mix/market/contracts?productType=USDT-FUTURES&"
            "symbol=DOGEUSDT");
  EXPECT_EQ(fetch.requests[1].url,
            "https://fixture.invalid/api/v2/spot/public/symbols?symbol=BTCUSDT");
  EXPECT_TRUE(connector.instrumentInfoError().empty()) << connector.instrumentInfoError();

  auto doge = registry.getSymbolInfo(*registry.getSymbolId("bitget", "DOGEUSDT"));
  ASSERT_TRUE(doge);
  EXPECT_TRUE(doge->tickSizeKnown);
  EXPECT_EQ(doge->tickSize.raw(), 1000);
  auto btc = registry.getSymbolInfo(*registry.getSymbolId("bitget", "BTCUSDT"));
  ASSERT_TRUE(btc);
  EXPECT_EQ(btc->tickSize.raw(), Price::fromDouble(0.01).raw());
  EXPECT_EQ(btc->type, InstrumentType::Spot);
}

TEST(InstrumentInfoLoad, HyperliquidPostsMetaAndRegistersEachCoin)
{
  Buses buses;
  SymbolRegistry registry;
  FixtureFetch fetch;
  fetch.answers.push_back({"/info", fixture("hyperliquid_meta.json")});

  HyperliquidConfig cfg;
  cfg.wsEndpoint = "wss://unused.invalid";
  cfg.instrumentInfoUrl = "https://fixture.invalid/info";
  cfg.instrumentFetch = fetch.fn();
  cfg.symbols = {"BTC", "DOGE", "NOTACOIN"};
  HyperliquidExchangeConnector connector(cfg, &buses.book, &buses.trade, &registry,
                                         makeLogger("hl_load.log"));
  connector.loadInstrumentSpecs();

  ASSERT_EQ(fetch.requests.size(), 1u);
  EXPECT_EQ(fetch.requests[0].body, R"({"type":"meta"})");

  auto btc = registry.getSymbolInfo(*registry.getSymbolId("hyperliquid", "BTC"));
  EXPECT_TRUE(btc->tickSizeKnown);
  EXPECT_EQ(btc->tickSize.raw(), Price::fromDouble(0.1).raw());
  auto doge = registry.getSymbolInfo(*registry.getSymbolId("hyperliquid", "DOGE"));
  EXPECT_EQ(doge->tickSize.raw(), 100);

  // A coin the universe does not list is said out loud and left unknown.
  EXPECT_NE(connector.instrumentInfoError().find("NOTACOIN"), std::string::npos)
      << connector.instrumentInfoError();
  auto missing = registry.getSymbolInfo(*registry.getSymbolId("hyperliquid", "NOTACOIN"));
  EXPECT_FALSE(missing->tickSizeKnown);
  EXPECT_EQ(orderTickRaw(*missing), 0);
}

TEST(InstrumentInfoLoad, PolymarketReadsEachTokensTick)
{
  Buses buses;
  SymbolRegistry registry;
  FixtureFetch fetch;
  fetch.answers.push_back({"token_id=FINE", fixture("polymarket_tick_0001.json")});
  fetch.answers.push_back({"token_id=GONE", fixture("polymarket_tick_not_found.json")});

  PolymarketConfig cfg;
  cfg.instrumentInfoUrl = "https://fixture.invalid";
  cfg.instrumentFetch = fetch.fn();
  cfg.tokenIds = {"FINE", "GONE"};
  PolymarketExchangeConnector connector(cfg, &buses.book, &buses.trade, &registry,
                                        makeLogger("pm_load.log"));
  connector.loadInstrumentSpecs();

  ASSERT_EQ(fetch.requests.size(), 2u);
  EXPECT_EQ(fetch.requests[0].url, "https://fixture.invalid/tick-size?token_id=FINE");
  auto fine = registry.getSymbolInfo(*registry.getSymbolId("polymarket", "FINE"));
  EXPECT_TRUE(fine->tickSizeKnown);
  EXPECT_EQ(fine->tickSize.raw(), 100000);
  EXPECT_EQ(orderLotRaw(*fine), Quantity::fromDouble(0.01).raw());
  ASSERT_TRUE(connector.instrumentSpec("FINE"));
  EXPECT_EQ(connector.instrumentSpec("FINE")->lotSize.raw(), Quantity::fromDouble(0.01).raw());
  auto gone = registry.getSymbolInfo(*registry.getSymbolId("polymarket", "GONE"));
  EXPECT_FALSE(gone->tickSizeKnown);
  EXPECT_EQ(orderLotRaw(*gone), 0) << "a token the venue does not list declares nothing";
  EXPECT_NE(connector.instrumentInfoError().find("market not found"), std::string::npos);
}

// ------------------------------------------------------------------ what a failure does

namespace
{

class Rejections final : public IOrderExecutionListener
{
 public:
  Rejections() : IOrderExecutionListener(21) {}
  void onOrderEvent(const OrderEvent& ev) override
  {
    std::lock_guard lock(_m);
    events.push_back(ev);
  }
  std::vector<OrderEvent> events;

 private:
  std::mutex _m;
};

class BodyTransport final : public ITransport
{
 public:
  void post(std::string_view, std::string_view body,
            const std::vector<std::pair<std::string_view, std::string_view>>&,
            MoveOnlyFunction<void(std::string_view)> onSuccess,
            MoveOnlyFunction<void(std::string_view)>) override
  {
    bodies.emplace_back(body);
    if (onSuccess)
    {
      onSuccess(R"({"retCode":0,"retMsg":"OK","result":{"orderId":"EX-1"}})");
    }
  }
  std::vector<std::string> bodies;
};

Order limitBuy(OrderId id, SymbolId sym, double px)
{
  Order o;
  o.id = id;
  o.symbol = sym;
  o.side = Side::BUY;
  o.type = OrderType::LIMIT;
  o.price = Price::fromDouble(px);
  o.quantity = Quantity::fromDouble(0.01);
  return o;
}

}  // namespace

// The venue's metadata could not be read: start() goes on (market data is
// still worth having), the failure is reported, and the symbol is registered
// with its tick unknown -- so an order price is neither checked nor rounded to
// the 0.01 placeholder, and goes out exactly as the strategy gave it.
TEST(InstrumentInfoLoad, AFailedLoadLeavesTheTickUnknownAndPricesUnrounded)
{
  Buses buses;
  SymbolRegistry registry;
  FixtureFetch fetch;
  fetch.answers.push_back({"symbol=BTCUSDT", std::unexpected<std::string>("HTTP 503: down")});

  BybitConfig cfg;
  cfg.publicEndpoint = "wss://unused.invalid";
  cfg.instrumentInfoUrl = "https://fixture.invalid";
  cfg.instrumentFetch = fetch.fn();
  cfg.symbols = {{"BTCUSDT", InstrumentType::Future, BybitConfig::BookDepth::Top50}};
  BybitExchangeConnector connector(cfg, &buses.book, &buses.trade, nullptr, &registry,
                                   makeLogger("bybit_fail.log"));
  connector.loadInstrumentSpecs();

  EXPECT_NE(connector.instrumentInfoError().find("HTTP 503"), std::string::npos);
  const SymbolId sym = *registry.getSymbolId("bybit", "BTCUSDT");
  EXPECT_FALSE(registry.getSymbolInfo(sym)->tickSizeKnown);

  BodyTransport transport;
  auto client =
      std::make_unique<AuthenticatedRestClient>("k", "s", "https://unused.invalid", &transport);
  OrderTracker tracker;
  BybitOrderExecutorT<NoPolicies> ex(std::move(client), &registry, &tracker);
  ex.submitOrder(limitBuy(1, sym, 60000.123));

  ASSERT_EQ(transport.bodies.size(), 1u);
  EXPECT_NE(transport.bodies[0].find(R"("price":"60000.123000")"), std::string::npos)
      << transport.bodies[0];
}

// With the tick loaded, the same off-tick price never leaves the process: it
// is refused with the reason the simulator gives, and an on-tick one goes out.
TEST(InstrumentInfoLoad, ALoadedTickRefusesOffTickPricesLikeTheSimulator)
{
  Buses buses;
  SymbolRegistry registry;
  FixtureFetch fetch;
  fetch.answers.push_back({"symbol=BTCUSDT", fixture("bybit_linear_btc.json")});

  BybitConfig cfg;
  cfg.publicEndpoint = "wss://unused.invalid";
  cfg.instrumentInfoUrl = "https://fixture.invalid";
  cfg.instrumentFetch = fetch.fn();
  cfg.symbols = {{"BTCUSDT", InstrumentType::Future, BybitConfig::BookDepth::Top50}};
  BybitExchangeConnector connector(cfg, &buses.book, &buses.trade, nullptr, &registry,
                                   makeLogger("bybit_tick.log"));
  connector.loadInstrumentSpecs();
  const SymbolId sym = *registry.getSymbolId("bybit", "BTCUSDT");

  BodyTransport transport;
  auto client =
      std::make_unique<AuthenticatedRestClient>("k", "s", "https://unused.invalid", &transport);
  OrderTracker tracker;
  OrderExecutionBus bus;
  Rejections rec;
  bus.subscribe(&rec);
  bus.start();
  BybitOrderExecutorT<NoPolicies> ex(std::move(client), &registry, &tracker);
  ex.setOrderBus(&bus);

  ex.submitOrder(limitBuy(1, sym, 60000.123));
  Order stop = limitBuy(2, sym, 0.0);
  stop.type = OrderType::STOP_MARKET;
  stop.side = Side::SELL;
  stop.triggerPrice = Price::fromDouble(58000.05);
  ex.submitOrder(stop);
  ex.submitOrder(limitBuy(3, sym, 60000.1));
  bus.flush();
  bus.stop();

  ASSERT_EQ(transport.bodies.size(), 1u);
  EXPECT_NE(transport.bodies[0].find(R"("orderLinkId":"3")"), std::string::npos);
  ASSERT_EQ(rec.events.size(), 2u);
  EXPECT_EQ(rec.events[0].status, OrderEventStatus::REJECTED);
  EXPECT_EQ(rec.events[0].rejectReason, kRejectPriceOffTick);
  EXPECT_EQ(rec.events[1].rejectReason, kRejectTriggerOffTick);
}

// The lot the same way: with it loaded, a quantity off the venue's step is
// refused with the simulator's reason and an on-lot one goes out.
TEST(InstrumentInfoLoad, ALoadedLotRefusesOffLotQuantitiesLikeTheSimulator)
{
  Buses buses;
  SymbolRegistry registry;
  FixtureFetch fetch;
  fetch.answers.push_back({"symbol=BTCUSDT", fixture("bybit_linear_btc.json")});

  BybitConfig cfg;
  cfg.publicEndpoint = "wss://unused.invalid";
  cfg.instrumentInfoUrl = "https://fixture.invalid";
  cfg.instrumentFetch = fetch.fn();
  cfg.symbols = {{"BTCUSDT", InstrumentType::Future, BybitConfig::BookDepth::Top50}};
  BybitExchangeConnector connector(cfg, &buses.book, &buses.trade, nullptr, &registry,
                                   makeLogger("bybit_lot.log"));
  connector.loadInstrumentSpecs();
  const SymbolId sym = *registry.getSymbolId("bybit", "BTCUSDT");

  BodyTransport transport;
  auto client =
      std::make_unique<AuthenticatedRestClient>("k", "s", "https://unused.invalid", &transport);
  OrderTracker tracker;
  OrderExecutionBus bus;
  Rejections rec;
  bus.subscribe(&rec);
  bus.start();
  BybitOrderExecutorT<NoPolicies> ex(std::move(client), &registry, &tracker);
  ex.setOrderBus(&bus);

  Order off = limitBuy(1, sym, 60000.1);
  off.quantity = Quantity::fromDouble(0.0015);
  ex.submitOrder(off);
  Order on = limitBuy(2, sym, 60000.1);
  on.quantity = Quantity::fromDouble(0.002);
  ex.submitOrder(on);
  bus.flush();
  bus.stop();

  ASSERT_EQ(transport.bodies.size(), 1u);
  EXPECT_NE(transport.bodies[0].find(R"("orderLinkId":"2")"), std::string::npos);
  ASSERT_EQ(rec.events.size(), 1u);
  EXPECT_EQ(rec.events[0].status, OrderEventStatus::REJECTED);
  EXPECT_EQ(rec.events[0].rejectReason, kRejectQuantityOffLot);
}

// ------------------------------------------------------------------ registered before start()

namespace
{

SymbolId preRegister(SymbolRegistry& registry, const std::string& exchange,
                     const std::string& symbol, std::optional<double> declaredTick = std::nullopt)
{
  SymbolInfo info;
  info.exchange = exchange;
  info.symbol = symbol;
  info.type = InstrumentType::Future;
  if (declaredTick)
  {
    info.tickSize = Price::fromDouble(*declaredTick);
    info.tickSizeKnown = true;
  }
  return registry.registerSymbol(info);
}

BybitConfig bybitBtcConfig(FixtureFetch& fetch)
{
  fetch.answers.push_back({"symbol=BTCUSDT", fixture("bybit_linear_btc.json")});
  BybitConfig cfg;
  cfg.publicEndpoint = "wss://unused.invalid";
  cfg.instrumentInfoUrl = "https://fixture.invalid";
  cfg.instrumentFetch = fetch.fn();
  cfg.symbols = {{"BTCUSDT", InstrumentType::Future, BybitConfig::BookDepth::Top50}};
  return cfg;
}

// A strategy that records the book it is handed, the one place an
// lrvx::Strategy reads a symbol's tick.
class BookReader final : public Strategy
{
 public:
  BookReader(SymbolId sym, const SymbolRegistry& registry) : Strategy(7, sym, registry) {}
  Price bookTick() const { return ctx().book.tickSize(); }
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

// The ordinary order of operations: the symbol is registered (with no tick),
// a strategy and the executor are built from its id, then the connector
// starts. The venue's tick reaches the registry entry they all share: the
// executor refuses an off-tick price with the simulator's reason and sends an
// on-tick one as its exact decimal.
TEST(InstrumentInfoPreRegistered, AnUndeclaredTickTakesTheVenues)
{
  Buses buses;
  SymbolRegistry registry;
  const SymbolId id = preRegister(registry, "bybit", "BTCUSDT");

  BodyTransport transport;
  auto client =
      std::make_unique<AuthenticatedRestClient>("k", "s", "https://unused.invalid", &transport);
  OrderTracker tracker;
  OrderExecutionBus bus;
  Rejections rec;
  bus.subscribe(&rec);
  bus.start();
  BybitOrderExecutorT<NoPolicies> ex(std::move(client), &registry, &tracker);
  ex.setOrderBus(&bus);

  FixtureFetch fetch;
  BybitExchangeConnector connector(bybitBtcConfig(fetch), &buses.book, &buses.trade, nullptr,
                                   &registry, makeLogger("bybit_prereg.log"));
  connector.loadInstrumentSpecs();

  EXPECT_TRUE(connector.instrumentInfoError().empty()) << connector.instrumentInfoError();
  EXPECT_EQ(*registry.getSymbolId("bybit", "BTCUSDT"), id);
  const auto info = registry.getSymbolInfo(id);
  EXPECT_TRUE(info->tickSizeKnown);
  EXPECT_EQ(info->tickSize.raw(), Price::fromDouble(0.1).raw());
  EXPECT_EQ(info->type, InstrumentType::Future);

  ex.submitOrder(limitBuy(1, id, 60000.123));
  ex.submitOrder(limitBuy(2, id, 60000.1));
  bus.flush();
  bus.stop();

  ASSERT_EQ(rec.events.size(), 1u);
  EXPECT_EQ(rec.events[0].order.id, 1u);
  EXPECT_EQ(rec.events[0].status, OrderEventStatus::REJECTED);
  EXPECT_EQ(rec.events[0].rejectReason, kRejectPriceOffTick);
  ASSERT_EQ(transport.bodies.size(), 1u);
  EXPECT_NE(transport.bodies[0].find(R"("orderLinkId":"2")"), std::string::npos);
  EXPECT_NE(transport.bodies[0].find(R"("price":"60000.100000")"), std::string::npos)
      << transport.bodies[0];
}

// Every connector applies it, each through its own load path.
TEST(InstrumentInfoPreRegistered, EveryVenueAppliesItsTick)
{
  Buses buses;
  SymbolRegistry registry;
  const SymbolId bitgetDoge = preRegister(registry, "bitget", "DOGEUSDT");
  const SymbolId hlBtc = preRegister(registry, "hyperliquid", "BTC");
  const SymbolId pmFine = preRegister(registry, "polymarket", "FINE");

  FixtureFetch fetch;
  fetch.answers.push_back({"/api/v2/mix/market/contracts", fixture("bitget_doge.json")});
  fetch.answers.push_back({"/info", fixture("hyperliquid_meta.json")});
  fetch.answers.push_back({"token_id=FINE", fixture("polymarket_tick_0001.json")});

  BitgetConfig bg;
  bg.publicEndpoint = "wss://unused.invalid";
  bg.instrumentInfoUrl = "https://fixture.invalid";
  bg.instrumentFetch = fetch.fn();
  bg.symbols = {{"DOGEUSDT", InstrumentType::Future, BitgetConfig::BookDepth::Depth15}};
  BitgetExchangeConnector bitget(bg, &buses.book, &buses.trade, nullptr, &registry,
                                 makeLogger("bitget_prereg.log"));
  bitget.loadInstrumentSpecs();

  HyperliquidConfig hl;
  hl.wsEndpoint = "wss://unused.invalid";
  hl.instrumentInfoUrl = "https://fixture.invalid/info";
  hl.instrumentFetch = fetch.fn();
  hl.symbols = {"BTC"};
  HyperliquidExchangeConnector hyperliquid(hl, &buses.book, &buses.trade, &registry,
                                           makeLogger("hl_prereg.log"));
  hyperliquid.loadInstrumentSpecs();

  PolymarketConfig pm;
  pm.instrumentInfoUrl = "https://fixture.invalid";
  pm.instrumentFetch = fetch.fn();
  pm.tokenIds = {"FINE"};
  PolymarketExchangeConnector polymarket(pm, &buses.book, &buses.trade, &registry,
                                         makeLogger("pm_prereg.log"));
  polymarket.loadInstrumentSpecs();

  EXPECT_TRUE(bitget.instrumentInfoError().empty()) << bitget.instrumentInfoError();
  EXPECT_TRUE(hyperliquid.instrumentInfoError().empty()) << hyperliquid.instrumentInfoError();
  EXPECT_TRUE(polymarket.instrumentInfoError().empty()) << polymarket.instrumentInfoError();
  EXPECT_EQ(orderTickRaw(*registry.getSymbolInfo(bitgetDoge)), 1000);
  EXPECT_EQ(orderTickRaw(*registry.getSymbolInfo(hlBtc)), Price::fromDouble(0.1).raw());
  EXPECT_EQ(orderTickRaw(*registry.getSymbolInfo(pmFine)), 100000);
  EXPECT_EQ(*registry.getSymbolId("polymarket", "FINE"), pmFine);

  // And its lot: Bitget's sizeMultiplier, Hyperliquid's 10^-szDecimals,
  // Polymarket's documented two size decimals.
  EXPECT_EQ(orderLotRaw(*registry.getSymbolInfo(bitgetDoge)), Quantity::fromDouble(1).raw());
  EXPECT_EQ(orderLotRaw(*registry.getSymbolInfo(hlBtc)), Quantity::fromDouble(0.00001).raw());
  EXPECT_EQ(orderLotRaw(*registry.getSymbolInfo(pmFine)), Quantity::fromDouble(0.01).raw());
}

// A declared lot equal to the venue's is left alone; one that differs is
// kept and reported, naming both values.
TEST(InstrumentInfoPreRegistered, ADeclaredLotIsKeptAndADifferenceReported)
{
  Buses buses;
  SymbolRegistry registry;
  SymbolInfo info;
  info.exchange = "bybit";
  info.symbol = "BTCUSDT";
  info.type = InstrumentType::Future;
  info.lotSize = Quantity::fromDouble(0.001);
  const SymbolId id = registry.registerSymbol(info);
  {
    FixtureFetch fetch;
    BybitExchangeConnector connector(bybitBtcConfig(fetch), &buses.book, &buses.trade, nullptr,
                                     &registry, makeLogger("bybit_prereg_lot_same.log"));
    connector.loadInstrumentSpecs();
    EXPECT_TRUE(connector.instrumentInfoError().empty()) << connector.instrumentInfoError();
    EXPECT_EQ(registry.getSymbolInfo(id)->lotSize.raw(), Quantity::fromDouble(0.001).raw());
  }

  SymbolRegistry other;
  info.lotSize = Quantity::fromDouble(0.01);
  const SymbolId otherId = other.registerSymbol(info);
  FixtureFetch fetch;
  BybitExchangeConnector connector(bybitBtcConfig(fetch), &buses.book, &buses.trade, nullptr,
                                   &other, makeLogger("bybit_prereg_lot_diff.log"));
  connector.loadInstrumentSpecs();
  EXPECT_EQ(other.getSymbolInfo(otherId)->lotSize.raw(), Quantity::fromDouble(0.01).raw());
  EXPECT_EQ(orderTickRaw(*other.getSymbolInfo(otherId)), Price::fromDouble(0.1).raw())
      << "the undeclared tick still takes the venue's";
  const std::string err = connector.instrumentInfoError();
  EXPECT_NE(err.find("BTCUSDT"), std::string::npos) << err;
  EXPECT_NE(err.find("lot 0.010000"), std::string::npos) << err;
  EXPECT_NE(err.find("0.001000"), std::string::npos) << err;
  EXPECT_EQ(err.find("tick"), std::string::npos) << "only the lot disagrees: " << err;
  EXPECT_EQ(connector.instrumentSpec("BTCUSDT")->lotSize.raw(), Quantity::fromDouble(0.001).raw());
}

// A tick and a lot that both disagree are one entry naming both.
TEST(InstrumentInfoPreRegistered, ATickAndALotThatBothDifferShareOneEntry)
{
  Buses buses;
  SymbolRegistry registry;
  SymbolInfo info;
  info.exchange = "bybit";
  info.symbol = "BTCUSDT";
  info.type = InstrumentType::Future;
  info.tickSize = Price::fromDouble(0.5);
  info.tickSizeKnown = true;
  info.lotSize = Quantity::fromDouble(0.01);
  registry.registerSymbol(info);
  FixtureFetch fetch;
  BybitExchangeConnector connector(bybitBtcConfig(fetch), &buses.book, &buses.trade, nullptr,
                                   &registry, makeLogger("bybit_prereg_both.log"));
  connector.loadInstrumentSpecs();
  const std::string err = connector.instrumentInfoError();
  EXPECT_NE(err.find("declares tick 0.500000"), std::string::npos) << err;
  EXPECT_NE(err.find("declares lot 0.010000"), std::string::npos) << err;
  EXPECT_EQ(err.find("BTCUSDT"), err.rfind("BTCUSDT")) << "one entry for the symbol: " << err;
}

// A declared tick equal to the venue's: nothing to do, nothing to report.
TEST(InstrumentInfoPreRegistered, ADeclaredTickEqualToTheVenuesIsLeftAlone)
{
  Buses buses;
  SymbolRegistry registry;
  const SymbolId id = preRegister(registry, "bybit", "BTCUSDT", 0.1);
  FixtureFetch fetch;
  BybitExchangeConnector connector(bybitBtcConfig(fetch), &buses.book, &buses.trade, nullptr,
                                   &registry, makeLogger("bybit_prereg_same.log"));
  connector.loadInstrumentSpecs();

  EXPECT_TRUE(connector.instrumentInfoError().empty()) << connector.instrumentInfoError();
  EXPECT_TRUE(registry.getSymbolInfo(id)->tickSizeKnown);
  EXPECT_EQ(registry.getSymbolInfo(id)->tickSize.raw(), Price::fromDouble(0.1).raw());
}

// A declared tick that differs from the venue's is the user's call: it is
// kept, and the disagreement is an error naming both values.
TEST(InstrumentInfoPreRegistered, ADeclaredTickThatDiffersIsKeptAndReported)
{
  Buses buses;
  SymbolRegistry registry;
  const SymbolId id = preRegister(registry, "bybit", "BTCUSDT", 0.5);
  FixtureFetch fetch;
  BybitExchangeConnector connector(bybitBtcConfig(fetch), &buses.book, &buses.trade, nullptr,
                                   &registry, makeLogger("bybit_prereg_diff.log"));
  connector.loadInstrumentSpecs();

  EXPECT_EQ(registry.getSymbolInfo(id)->tickSize.raw(), Price::fromDouble(0.5).raw());
  EXPECT_TRUE(registry.getSymbolInfo(id)->tickSizeKnown);
  const std::string err = connector.instrumentInfoError();
  EXPECT_NE(err.find("BTCUSDT"), std::string::npos) << err;
  EXPECT_NE(err.find("0.500000"), std::string::npos) << err;
  EXPECT_NE(err.find("0.100000"), std::string::npos) << err;
  // The venue's value stays available to the caller.
  EXPECT_EQ(connector.instrumentSpec("BTCUSDT")->tickSize.raw(), Price::fromDouble(0.1).raw());
}

// A symbol first seen after the load is registered with the venue's tick, as
// before; one the connector does not serve keeps its entry untouched.
TEST(InstrumentInfoPreRegistered, SymbolsTheConnectorDoesNotOwnAreUntouched)
{
  Buses buses;
  SymbolRegistry registry;
  const SymbolId eth = preRegister(registry, "bybit", "ETHUSDT");
  const SymbolId otherVenue = preRegister(registry, "bitget", "BTCUSDT");
  FixtureFetch fetch;
  BybitExchangeConnector connector(bybitBtcConfig(fetch), &buses.book, &buses.trade, nullptr,
                                   &registry, makeLogger("bybit_prereg_other.log"));
  connector.loadInstrumentSpecs();

  const SymbolId btc = connector.resolveSymbolId("BTCUSDT");
  EXPECT_EQ(orderTickRaw(*registry.getSymbolInfo(btc)), Price::fromDouble(0.1).raw());
  EXPECT_FALSE(registry.getSymbolInfo(eth)->tickSizeKnown);
  EXPECT_FALSE(registry.getSymbolInfo(otherVenue)->tickSizeKnown);
  EXPECT_EQ(registry.getSymbolInfo(otherVenue)->tickSize.raw(), kDefaultTickSize.raw());
}

// A strategy built before the connector starts sizes its book on the venue's
// tick by the first frame. DOGE's 0.00001 is finer than the 0.01 placeholder:
// on the placeholder's grid two neighbouring venue levels fall into one
// bucket and the best bid is quantised away.
TEST(InstrumentInfoPreRegistered, AStrategyBuiltBeforeStartBooksOnTheVenueTick)
{
  Buses buses;
  SymbolRegistry registry;
  const SymbolId doge = preRegister(registry, "bitget", "DOGEUSDT");
  BookReader strategy(doge, registry);

  FixtureFetch fetch;
  fetch.answers.push_back({"/api/v2/mix/market/contracts", fixture("bitget_doge.json")});
  BitgetConfig bg;
  bg.publicEndpoint = "wss://unused.invalid";
  bg.instrumentInfoUrl = "https://fixture.invalid";
  bg.instrumentFetch = fetch.fn();
  bg.symbols = {{"DOGEUSDT", InstrumentType::Future, BitgetConfig::BookDepth::Depth15}};
  BitgetExchangeConnector connector(bg, &buses.book, &buses.trade, nullptr, &registry,
                                    makeLogger("bitget_prereg_strategy.log"));
  connector.loadInstrumentSpecs();

  feedSnapshot(strategy, doge, {0.10002, 0.10001}, 0.10004);
  ASSERT_TRUE(strategy.seenTick);
  EXPECT_EQ(strategy.seenTick->raw(), 1000);
  EXPECT_EQ(strategy.bookTick().raw(), 1000);
  ASSERT_TRUE(strategy.seenBestBid);
  EXPECT_EQ(strategy.seenBestBid->raw(), Price::fromDouble(0.10002).raw());
}

// The guarantee start() gives: when it returns, every configured symbol --
// registered before it or not -- carries the venue's tick, or
// instrumentInfoError() says why not. Orders can only follow.
TEST(InstrumentInfoPreRegistered, ByTheTimeStartReturnsTheTickIsApplied)
{
  Buses buses;
  SymbolRegistry registry;
  const SymbolId btc = preRegister(registry, "bybit", "BTCUSDT");
  FixtureFetch fetch;
  fetch.answers.push_back({"symbol=ETHUSDT", fixture("bybit_symbol_invalid.json")});
  BybitConfig cfg = bybitBtcConfig(fetch);
  cfg.symbols.push_back({"ETHUSDT", InstrumentType::Future, BybitConfig::BookDepth::Top50});
  cfg.publicEndpoint = test::refusingUrl();
  cfg.reconnectDelayMs = 5000;
  BybitExchangeConnector connector(cfg, &buses.book, &buses.trade, nullptr, &registry,
                                   makeLogger("bybit_prereg_start.log"));
  connector.start();
  const auto btcInfo = registry.getSymbolInfo(btc);
  const std::string err = connector.instrumentInfoError();
  connector.stop();

  EXPECT_TRUE(btcInfo->tickSizeKnown);
  EXPECT_EQ(btcInfo->tickSize.raw(), Price::fromDouble(0.1).raw());
  EXPECT_NE(err.find("ETHUSDT"), std::string::npos) << err;
  EXPECT_EQ(err.find("BTCUSDT"), std::string::npos) << err;
}

// An unreachable venue is asked once: each further symbol would wait out the
// same timeout inside start(). Every symbol is still reported as missing.
TEST(InstrumentInfoLoad, AnUnreachableVenueIsAskedOnce)
{
  Buses buses;
  SymbolRegistry registry;
  FixtureFetch fetch;
  fetch.answers.push_back(
      {"instruments-info", std::unexpected<std::string>("Timeout was reached")});

  BybitConfig cfg;
  cfg.publicEndpoint = "wss://unused.invalid";
  cfg.instrumentInfoUrl = "https://fixture.invalid";
  cfg.instrumentFetch = fetch.fn();
  cfg.symbols = {{"BTCUSDT", InstrumentType::Future, BybitConfig::BookDepth::Top50},
                 {"ETHUSDT", InstrumentType::Future, BybitConfig::BookDepth::Top50}};
  BybitExchangeConnector connector(cfg, &buses.book, &buses.trade, nullptr, &registry,
                                   makeLogger("bybit_unreachable.log"));
  connector.loadInstrumentSpecs();

  EXPECT_EQ(fetch.requests.size(), 1u);
  const std::string err = connector.instrumentInfoError();
  EXPECT_NE(err.find("BTCUSDT: Timeout was reached"), std::string::npos) << err;
  EXPECT_NE(err.find("ETHUSDT: not requested"), std::string::npos) << err;
  EXPECT_FALSE(registry.getSymbolInfo(*registry.getSymbolId("bybit", "ETHUSDT"))->tickSizeKnown);
}
