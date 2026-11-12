/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Instrument info over the network: each connector, with its default
 * instrumentInfoUrl and the default libcurl fetch, runs loadInstrumentSpecs()
 * (the step start() runs before it opens any socket) against the venue's
 * public, unauthenticated endpoint. The test then reads the same endpoint
 * itself and derives the tick and size step with its own string handling, so
 * a format change on the venue's side shows up as a mismatch or a load error
 * here, where the offline tests only replay recorded bodies.
 *
 * No socket is opened, no credential is used, and nothing is sent but GETs
 * and Hyperliquid's {"type":"meta"} info request.
 */

#include "lrvx-connectors/bitget/bitget_exchange_connector.h"
#include "lrvx-connectors/bybit/bybit_exchange_connector.h"
#include "lrvx-connectors/hyperliquid/hyperliquid_exchange_connector.h"
#include "lrvx-connectors/polymarket/polymarket_exchange_connector.h"

#include <lrvx/book/bus/book_update_bus.h>
#include <lrvx/book/bus/trade_bus.h>
#include <lrvx/common.h>
#include <lrvx/execution/order_price_rule.h>
#include <lrvx/log/atomic_logger.h>

#include <curl/curl.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

using namespace lrvx;

namespace
{

constexpr int64_t kScale = 100'000'000;  // Price::Scale and Quantity::Scale

size_t appendBody(char* ptr, size_t size, size_t nmemb, void* userdata)
{
  static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
  return size * nmemb;
}

// The test's own request, independent of the connector's fetch.
std::string httpRequest(const std::string& url, const std::string& postBody = {})
{
  std::string out;
  CURL* h = curl_easy_init();
  if (!h)
  {
    return out;
  }
  curl_easy_setopt(h, CURLOPT_URL, url.c_str());
  curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, 15000L);
  curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, appendBody);
  curl_easy_setopt(h, CURLOPT_WRITEDATA, &out);
  curl_slist* hdrs = nullptr;
  if (!postBody.empty())
  {
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, postBody.c_str());
  }
  curl_easy_perform(h);
  curl_slist_free_all(hdrs);
  curl_easy_cleanup(h);
  return out;
}

// The text after `"key":` starting at `from`, unquoted: "0.10" or 0.001.
std::optional<std::string> jsonValue(std::string_view body, std::string_view key, size_t from = 0)
{
  const std::string needle = "\"" + std::string(key) + "\":";
  size_t pos = body.find(needle, from);
  if (pos == std::string_view::npos)
  {
    return std::nullopt;
  }
  pos += needle.size();
  while (pos < body.size() && body[pos] == ' ')
  {
    ++pos;
  }
  if (pos < body.size() && body[pos] == '"')
  {
    const size_t end = body.find('"', pos + 1);
    return std::string(body.substr(pos + 1, end - pos - 1));
  }
  size_t end = pos;
  while (end < body.size() && body[end] != ',' && body[end] != '}' && body[end] != ']')
  {
    ++end;
  }
  return std::string(body.substr(pos, end - pos));
}

// "0.10" -> 10'000'000 at scale 1e8, by digits.
int64_t decimalRaw(std::string_view text)
{
  int64_t intPart = 0;
  int64_t frac = 0;
  int64_t fracScale = kScale;
  bool afterDot = false;
  for (char c : text)
  {
    if (c == '.')
    {
      afterDot = true;
      continue;
    }
    if (c < '0' || c > '9')
    {
      return -1;
    }
    if (!afterDot)
    {
      intPart = intPart * 10 + (c - '0');
    }
    else
    {
      fracScale /= 10;
      frac += (c - '0') * fracScale;
    }
  }
  return intPart * kScale + frac;
}

int64_t tenToMinusRaw(int64_t digits)
{
  int64_t raw = kScale;
  for (int64_t i = 0; i < digits; ++i)
  {
    raw /= 10;
  }
  return raw;
}

std::shared_ptr<AtomicLogger> makeLogger(const char* name)
{
  auto dir = std::filesystem::temp_directory_path() / "lrvx_instrument_info_network_logs";
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

template <typename Connector>
void expectVenueTick(const Connector& connector, const SymbolRegistry& registry,
                     const std::string& exchange, const std::string& symbol, int64_t tickRaw,
                     std::optional<int64_t> lotRaw)
{
  EXPECT_TRUE(connector.instrumentInfoError().empty()) << connector.instrumentInfoError();
  auto id = registry.getSymbolId(exchange, symbol);
  ASSERT_TRUE(id) << exchange << " " << symbol << " not registered";
  auto info = registry.getSymbolInfo(*id);
  ASSERT_TRUE(info);
  auto spec = connector.instrumentSpec(symbol);
  ASSERT_TRUE(spec);
  std::cout << "[" << exchange << " " << symbol.substr(0, 24) << "] registered tick "
            << info->tickSize.toString() << " (known=" << info->tickSizeKnown << "), lot "
            << spec->lotSize.toString() << "; venue publishes tick "
            << Price::fromRaw(tickRaw).toString() << ", lot "
            << (lotRaw ? Quantity::fromRaw(*lotRaw).toString() : std::string("n/a")) << "\n";
  EXPECT_TRUE(info->tickSizeKnown);
  EXPECT_EQ(info->tickSize.raw(), tickRaw);
  EXPECT_EQ(orderTickRaw(*info), tickRaw);
  if (lotRaw)
  {
    EXPECT_EQ(spec->lotSize.raw(), *lotRaw);
  }
}

template <typename Connector>
void expectTickUnknown(const Connector& connector, const SymbolRegistry& registry,
                       const std::string& exchange, const std::string& symbol,
                       std::string_view venueText)
{
  const std::string err = connector.instrumentInfoError();
  std::cout << "[" << exchange << " " << symbol << "] instrumentInfoError: " << err << "\n";
  EXPECT_NE(err.find(symbol), std::string::npos) << err;
  EXPECT_NE(err.find(venueText), std::string::npos) << err;
  auto id = registry.getSymbolId(exchange, symbol);
  ASSERT_TRUE(id) << exchange << " " << symbol << " not registered";
  auto info = registry.getSymbolInfo(*id);
  ASSERT_TRUE(info);
  EXPECT_FALSE(info->tickSizeKnown);
  EXPECT_EQ(orderTickRaw(*info), 0) << "an unknown tick must not be declared as the placeholder";
  EXPECT_FALSE(connector.instrumentSpec(symbol));
}

}  // namespace

// ------------------------------------------------------------------ Bybit

static void bybitCase(InstrumentType type, const char* category)
{
  const std::string body =
      httpRequest(std::string("https://api.bybit.com/v5/market/instruments-info?category=") +
                  category + "&symbol=BTCUSDT");
  ASSERT_EQ(jsonValue(body, "retCode").value_or(""), "0") << body.substr(0, 400);
  const size_t priceFilter = body.find("\"priceFilter\"");
  const size_t lotFilter = body.find("\"lotSizeFilter\"");
  ASSERT_NE(priceFilter, std::string::npos) << body.substr(0, 400);
  ASSERT_NE(lotFilter, std::string::npos) << body.substr(0, 400);
  const auto tick = jsonValue(body, "tickSize", priceFilter);
  const auto lot =
      jsonValue(body, type == InstrumentType::Spot ? "basePrecision" : "qtyStep", lotFilter);
  ASSERT_TRUE(tick && lot) << body.substr(0, 400);

  Buses buses;
  SymbolRegistry registry;
  BybitConfig cfg;
  cfg.symbols = {{"BTCUSDT", type, BybitConfig::BookDepth::Top1}};
  BybitExchangeConnector connector(cfg, &buses.book, &buses.trade, nullptr, &registry,
                                   makeLogger("bybit_net.log"));
  connector.loadInstrumentSpecs();
  expectVenueTick(connector, registry, "bybit", "BTCUSDT", decimalRaw(*tick), decimalRaw(*lot));
}

TEST(InstrumentInfoNetwork, BybitLinear) { bybitCase(InstrumentType::Future, "linear"); }

TEST(InstrumentInfoNetwork, BybitSpot) { bybitCase(InstrumentType::Spot, "spot"); }

TEST(InstrumentInfoNetwork, BybitUnknownSymbolStaysTickUnknown)
{
  Buses buses;
  SymbolRegistry registry;
  BybitConfig cfg;
  cfg.symbols = {{"NOPEUSDT", InstrumentType::Future, BybitConfig::BookDepth::Top1}};
  BybitExchangeConnector connector(cfg, &buses.book, &buses.trade, nullptr, &registry,
                                   makeLogger("bybit_net_unknown.log"));
  connector.loadInstrumentSpecs();
  expectTickUnknown(connector, registry, "bybit", "NOPEUSDT", "symbol invalid");
}

// ------------------------------------------------------------------ Bitget

TEST(InstrumentInfoNetwork, BitgetFutures)
{
  const std::string body = httpRequest(
      "https://api.bitget.com/api/v2/mix/market/contracts?productType=USDT-FUTURES&symbol=BTCUSDT");
  ASSERT_EQ(jsonValue(body, "code").value_or(""), "00000") << body.substr(0, 400);
  const auto place = jsonValue(body, "pricePlace");
  const auto endStep = jsonValue(body, "priceEndStep");
  const auto lot = jsonValue(body, "sizeMultiplier");
  ASSERT_TRUE(place && endStep && lot) << body.substr(0, 400);
  const int64_t tick = tenToMinusRaw(std::stoll(*place)) * std::stoll(*endStep);

  Buses buses;
  SymbolRegistry registry;
  BitgetConfig cfg;
  cfg.symbols = {{"BTCUSDT", InstrumentType::Future, BitgetConfig::BookDepth::Depth1}};
  BitgetExchangeConnector connector(cfg, &buses.book, &buses.trade, nullptr, &registry,
                                    makeLogger("bitget_net.log"));
  connector.loadInstrumentSpecs();
  expectVenueTick(connector, registry, "bitget", "BTCUSDT", tick, decimalRaw(*lot));
}

TEST(InstrumentInfoNetwork, BitgetSpot)
{
  const std::string body =
      httpRequest("https://api.bitget.com/api/v2/spot/public/symbols?symbol=BTCUSDT");
  ASSERT_EQ(jsonValue(body, "code").value_or(""), "00000") << body.substr(0, 400);
  const auto pricePrecision = jsonValue(body, "pricePrecision");
  const auto qtyPrecision = jsonValue(body, "quantityPrecision");
  ASSERT_TRUE(pricePrecision && qtyPrecision) << body.substr(0, 400);

  Buses buses;
  SymbolRegistry registry;
  BitgetConfig cfg;
  cfg.symbols = {{"BTCUSDT", InstrumentType::Spot, BitgetConfig::BookDepth::Depth1}};
  BitgetExchangeConnector connector(cfg, &buses.book, &buses.trade, nullptr, &registry,
                                    makeLogger("bitget_spot_net.log"));
  connector.loadInstrumentSpecs();
  expectVenueTick(connector, registry, "bitget", "BTCUSDT",
                  tenToMinusRaw(std::stoll(*pricePrecision)),
                  tenToMinusRaw(std::stoll(*qtyPrecision)));
}

TEST(InstrumentInfoNetwork, BitgetUnknownSymbolStaysTickUnknown)
{
  Buses buses;
  SymbolRegistry registry;
  BitgetConfig cfg;
  cfg.symbols = {{"NOPEUSDT", InstrumentType::Future, BitgetConfig::BookDepth::Depth1}};
  BitgetExchangeConnector connector(cfg, &buses.book, &buses.trade, nullptr, &registry,
                                    makeLogger("bitget_net_unknown.log"));
  connector.loadInstrumentSpecs();
  expectTickUnknown(connector, registry, "bitget", "NOPEUSDT", "does not exist");
}

// ------------------------------------------------------------------ Hyperliquid

TEST(InstrumentInfoNetwork, HyperliquidBtc)
{
  const std::string body = httpRequest("https://api.hyperliquid.xyz/info", R"({"type":"meta"})");
  // {"szDecimals":5,"name":"BTC",...}: the object that names BTC.
  const size_t name = body.find("\"name\":\"BTC\"");
  ASSERT_NE(name, std::string::npos) << body.substr(0, 400);
  const size_t objStart = body.rfind('{', name);
  const auto sz = jsonValue(body, "szDecimals", objStart);
  ASSERT_TRUE(sz) << body.substr(0, 400);
  const int64_t szDecimals = std::stoll(*sz);

  Buses buses;
  SymbolRegistry registry;
  HyperliquidConfig cfg;
  cfg.symbols = {"BTC"};
  HyperliquidExchangeConnector connector(cfg, &buses.book, &buses.trade, &registry,
                                         makeLogger("hl_net.log"));
  connector.loadInstrumentSpecs();
  expectVenueTick(connector, registry, "hyperliquid", "BTC", tenToMinusRaw(6 - szDecimals),
                  tenToMinusRaw(szDecimals));
}

TEST(InstrumentInfoNetwork, HyperliquidUnknownCoinStaysTickUnknown)
{
  Buses buses;
  SymbolRegistry registry;
  HyperliquidConfig cfg;
  cfg.symbols = {"NOTACOIN"};
  HyperliquidExchangeConnector connector(cfg, &buses.book, &buses.trade, &registry,
                                         makeLogger("hl_net_unknown.log"));
  connector.loadInstrumentSpecs();
  expectTickUnknown(connector, registry, "hyperliquid", "NOTACOIN", "not listed");
}

// ------------------------------------------------------------------ Polymarket

// Tokens expire with their markets, so the token is the first one of a market
// the CLOB currently lists as active and accepting orders.
static std::string activePolymarketToken()
{
  const std::string body = httpRequest("https://clob.polymarket.com/sampling-markets");
  constexpr std::string_view kMarket = "{\"enable_order_book\"";
  size_t start = body.find(kMarket);
  while (start != std::string::npos)
  {
    const size_t next = body.find(kMarket, start + 1);
    const std::string_view market = std::string_view(body).substr(
        start, next == std::string::npos ? std::string::npos : next - start);
    if (market.find("\"active\":true") != std::string_view::npos &&
        market.find("\"closed\":false") != std::string_view::npos &&
        market.find("\"accepting_orders\":true") != std::string_view::npos)
    {
      const size_t tokens = market.find("\"tokens\":[");
      if (tokens != std::string_view::npos)
      {
        if (auto token = jsonValue(market, "token_id", tokens); token && !token->empty())
        {
          return *token;
        }
      }
    }
    start = next;
  }
  return {};
}

TEST(InstrumentInfoNetwork, PolymarketActiveToken)
{
  const std::string token = activePolymarketToken();
  ASSERT_FALSE(token.empty()) << "no active market in /sampling-markets";
  const std::string body = httpRequest("https://clob.polymarket.com/tick-size?token_id=" + token);
  const auto tick = jsonValue(body, "minimum_tick_size");
  ASSERT_TRUE(tick) << body.substr(0, 400);

  Buses buses;
  SymbolRegistry registry;
  PolymarketConfig cfg;
  cfg.tokenIds = {token};
  PolymarketExchangeConnector connector(cfg, &buses.book, &buses.trade, &registry,
                                        makeLogger("pm_net.log"));
  connector.loadInstrumentSpecs();
  expectVenueTick(connector, registry, "polymarket", token, decimalRaw(*tick), std::nullopt);
}

TEST(InstrumentInfoNetwork, PolymarketUnknownTokenStaysTickUnknown)
{
  Buses buses;
  SymbolRegistry registry;
  PolymarketConfig cfg;
  cfg.tokenIds = {"123"};
  PolymarketExchangeConnector connector(cfg, &buses.book, &buses.trade, &registry,
                                        makeLogger("pm_net_unknown.log"));
  connector.loadInstrumentSpecs();
  expectTickUnknown(connector, registry, "polymarket", "123", "market not found");
}
