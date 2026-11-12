/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * The exact request each executor sends for orders whose prices are on the
 * instrument's tick. Recorded from the executors as they were before the
 * venue tick reached the registry, and compared byte for byte since: knowing
 * the tick may refuse an off-tick price, it must never change what goes out
 * for an on-tick one. Each case runs twice, once with the symbol's tick
 * unknown (the registry default every live symbol used to get) and once with
 * the venue's tick declared.
 */

#include "lrvx-connectors/bitget/authenticated_rest_client.h"
#include "lrvx-connectors/bitget/bitget_order_executor.h"
#include "lrvx-connectors/bybit/authenticated_rest_client.h"
#include "lrvx-connectors/bybit/bybit_order_executor.h"
#include "lrvx-connectors/hyperliquid/hl_signer.h"
#include "lrvx-connectors/hyperliquid/hyperliquid_order_executor.h"

#include <lrvx/common.h>
#include <lrvx/engine/symbol_registry.h>
#include <lrvx/execution/order.h>
#include <lrvx/execution/order_tracker.h>
#include <lrvx/log/atomic_logger.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace lrvx::hl
{
std::optional<HlSig> hl_sign_with_sdk(const HlSignParams&) { return HlSig{"0xr", "0xs", 27}; }
}  // namespace lrvx::hl

using namespace lrvx;

namespace
{

class FakeTransport final : public ITransport
{
 public:
  void post(std::string_view url, std::string_view body,
            const std::vector<std::pair<std::string_view, std::string_view>>&,
            MoveOnlyFunction<void(std::string_view)> onSuccess,
            MoveOnlyFunction<void(std::string_view)>) override
  {
    if (url.ends_with("/info"))
    {
      if (onSuccess)
      {
        onSuccess(R"({"universe":[{"szDecimals":5,"name":"BTC"},{"szDecimals":0,"name":"DOGE"}]})");
      }
      return;
    }
    bodies.emplace_back(body);
    if (onSuccess)
    {
      onSuccess(response);
    }
  }

  std::vector<std::string> bodies;
  std::string response;
};

SymbolId registerSymbol(SymbolRegistry& registry, const std::string& exchange,
                        const std::string& symbol, std::optional<double> tick)
{
  SymbolInfo info;
  info.exchange = exchange;
  info.symbol = symbol;
  info.type = InstrumentType::Future;
  if (tick)
  {
    info.tickSize = Price::fromDouble(*tick);
    info.tickSizeKnown = true;
  }
  return registry.registerSymbol(info);
}

Order limit(OrderId id, SymbolId sym, Side side, double px, double qty)
{
  Order o;
  o.id = id;
  o.symbol = sym;
  o.side = side;
  o.type = OrderType::LIMIT;
  o.price = Price::fromDouble(px);
  o.quantity = Quantity::fromDouble(qty);
  return o;
}

Order conditional(OrderId id, SymbolId sym, Side side, OrderType type, double trigger, double px,
                  double qty)
{
  Order o = limit(id, sym, side, px, qty);
  o.type = type;
  o.triggerPrice = Price::fromDouble(trigger);
  return o;
}

std::vector<std::string> bybitBodies(std::optional<double> tick)
{
  auto transport = std::make_unique<FakeTransport>();
  auto* raw = transport.get();
  raw->response = R"({"retCode":0,"retMsg":"OK","result":{"orderId":"EX-1"}})";
  auto client = std::make_unique<AuthenticatedRestClient>("k", "s", "https://api.bybit.com", raw);
  SymbolRegistry registry;
  OrderTracker tracker;
  const SymbolId sym = registerSymbol(registry, "bybit", "BTCUSDT", tick);
  BybitOrderExecutorT<NoPolicies> ex(std::move(client), &registry, &tracker);

  ex.submitOrder(limit(1, sym, Side::BUY, 60000.5, 0.012));
  Order mkt = limit(2, sym, Side::SELL, 0.0, 0.5);
  mkt.type = OrderType::MARKET;
  ex.submitOrder(mkt);
  ex.submitOrder(conditional(3, sym, Side::SELL, OrderType::STOP_MARKET, 58000.1, 0.0, 1.0));
  ex.submitOrder(conditional(4, sym, Side::SELL, OrderType::STOP_LIMIT, 58000.1, 57990.3, 1.0));
  ex.submitOrder(
      conditional(5, sym, Side::SELL, OrderType::TAKE_PROFIT_LIMIT, 65000.7, 65010.2, 1.0));
  Order amended = limit(6, sym, Side::BUY, 60001.4, 0.02);
  ex.replaceOrder(1, amended);
  return raw->bodies;
}

std::vector<std::string> bitgetBodies(std::optional<double> tick)
{
  auto transport = std::make_unique<FakeTransport>();
  auto* raw = transport.get();
  raw->response = R"({"code":"00000","msg":"success","data":{"orderId":"BG-1"}})";
  auto client =
      std::make_unique<BitgetAuthenticatedRestClient>("k", "s", "p", "https://api.bitget.com", raw);
  SymbolRegistry registry;
  OrderTracker tracker;
  const SymbolId sym = registerSymbol(registry, "bitget", "BTCUSDT", tick);
  Bitget::Params params;
  params.productType = "USDT-FUTURES";
  params.marginCoin = "USDT";
  params.marginMode = "crossed";
  BitgetOrderExecutorT<NoPolicies> ex(std::move(client), &registry, &tracker, params);

  ex.submitOrder(limit(1, sym, Side::BUY, 60000.5, 0.012));
  ex.submitOrder(conditional(2, sym, Side::SELL, OrderType::STOP_MARKET, 58000.1, 0.0, 1.0));
  ex.submitOrder(conditional(3, sym, Side::SELL, OrderType::STOP_LIMIT, 58000.1, 57990.3, 1.0));
  ex.replaceOrder(1, limit(4, sym, Side::BUY, 60001.4, 0.02));
  raw->response = R"({"code":"00000","msg":"success","data":[{"orderId":"TP-1"}]})";
  ex.placePosTpsl(sym, HoldSide::Long, 58000.1, 65000.7, 5);
  raw->response = R"({"code":"00000","msg":"success","data":{}})";
  ex.modifyPosTpsl(sym, "TP-1", 58100.2, 1.0);
  return raw->bodies;
}

std::string tempLogDir()
{
  auto dir = std::filesystem::temp_directory_path() / "lrvx_on_tick_pin_logs";
  std::filesystem::create_directories(dir);
  return dir.string();
}

std::vector<std::string> hyperliquidBodies(std::optional<double> btcTick)
{
  auto transport = std::make_unique<FakeTransport>();
  auto* raw = transport.get();
  raw->response =
      R"({"status":"ok","response":{"type":"order","data":{"statuses":[{"resting":{"oid":777}}]}}})";
  SymbolRegistry registry;
  OrderTracker tracker;
  const SymbolId sym = registerSymbol(registry, "hyperliquid", "BTC", btcTick);
  const SymbolId doge = registerSymbol(registry, "hyperliquid", "DOGE",
                                       btcTick ? std::optional<double>(0.000001) : std::nullopt);
  AtomicLoggerOptions logOpts;
  logOpts.directory = tempLogDir();
  logOpts.basename = "pins.log";
  HyperliquidOrderExecutorT<NoPolicies> ex(
      std::move(transport), "https://unused.invalid", "aa", &registry, &tracker,
      std::make_shared<AtomicLogger>(logOpts), "0xacct", std::nullopt, true);

  ex.submitOrder(limit(1, sym, Side::BUY, 60000.0, 0.012));
  ex.submitOrder(limit(2, sym, Side::SELL, 101234.0, 0.5));
  Order mkt = limit(3, sym, Side::BUY, 63000.0, 0.01);
  mkt.type = OrderType::MARKET;
  ex.submitOrder(mkt);
  ex.submitOrder(conditional(4, sym, Side::SELL, OrderType::STOP_MARKET, 58000.0, 0.0, 1.0));
  ex.submitOrder(conditional(5, sym, Side::SELL, OrderType::STOP_LIMIT, 58001.0, 57990.0, 1.0));
  ex.submitOrder(
      conditional(6, sym, Side::SELL, OrderType::TAKE_PROFIT_LIMIT, 65000.0, 65010.0, 1.0));
  ex.submitOrder(limit(7, doge, Side::BUY, 0.12345, 100.0));
  ex.replaceOrder(1, limit(8, sym, Side::BUY, 60001.0, 0.02));
  return raw->bodies;
}

// The HL bodies carry a nonce from the wall clock and a random cloid; both
// are masked so the rest can be compared exactly.
std::string maskField(std::string body, const std::string& key, const char* chars)
{
  for (auto pos = body.find(key); pos != std::string::npos; pos = body.find(key, pos + 1))
  {
    const auto start = pos + key.size();
    const auto end = body.find_first_not_of(chars, start);
    body.replace(start, end - start, "X");
  }
  return body;
}

std::string maskNonce(std::string body)
{
  body = maskField(std::move(body), "\"nonce\":", "0123456789");
  return maskField(std::move(body), "\"c\":\"0x", "0123456789abcdef");
}

}  // namespace

TEST(OnTickRequestPins, Bybit)
{
  const std::vector<std::string> expected = {
      R"({"category":"linear","symbol":"BTCUSDT","orderLinkId":"1","side":"Buy","orderType":"Limit","qty":"0.012000","price":"60000.500000","timeInForce":"GTC","reduceOnly":false})",
      R"({"category":"linear","symbol":"BTCUSDT","orderLinkId":"2","side":"Sell","orderType":"Market","qty":"0.500000","timeInForce":"GTC","reduceOnly":false})",
      R"({"category":"linear","symbol":"BTCUSDT","orderLinkId":"3","side":"Sell","orderType":"Market","qty":"1.000000","timeInForce":"GTC","reduceOnly":false,"triggerPrice":"58000.100000","triggerDirection":2})",
      R"({"category":"linear","symbol":"BTCUSDT","orderLinkId":"4","side":"Sell","orderType":"Limit","qty":"1.000000","price":"57990.300000","timeInForce":"GTC","reduceOnly":false,"triggerPrice":"58000.100000","triggerDirection":2})",
      R"({"category":"linear","symbol":"BTCUSDT","orderLinkId":"5","side":"Sell","orderType":"Limit","qty":"1.000000","price":"65010.200000","timeInForce":"GTC","reduceOnly":false,"triggerPrice":"65000.700000","triggerDirection":1})",
      R"({"category":"linear","symbol":"BTCUSDT","orderId":"EX-1","qty":"0.020000","price":"60001.400000"})",
  };
  EXPECT_EQ(bybitBodies(std::nullopt), expected);
  EXPECT_EQ(bybitBodies(0.1), expected);
}

TEST(OnTickRequestPins, Bitget)
{
  const std::vector<std::string> expected = {
      R"({"symbol":"BTCUSDT","productType":"USDT-FUTURES","marginMode":"crossed","marginCoin":"USDT","size":"0.012000","price":"60000.500000","side":"buy","orderType":"limit","force":"gtc","clientOid":"1"})",
      R"({"planType":"normal_plan","symbol":"BTCUSDT","productType":"USDT-FUTURES","marginMode":"crossed","marginCoin":"USDT","size":"1.000000","triggerPrice":"58000.1","triggerType":"mark_price","side":"sell","orderType":"market","clientOid":"2"})",
      R"({"planType":"normal_plan","symbol":"BTCUSDT","productType":"USDT-FUTURES","marginMode":"crossed","marginCoin":"USDT","size":"1.000000","triggerPrice":"58000.1","triggerType":"mark_price","side":"sell","orderType":"limit","price":"57990.3","clientOid":"3"})",
      R"({"orderId":"BG-1","symbol":"BTCUSDT","productType":"USDT-FUTURES","marginCoin":"USDT","newPrice":"60001.400000","newSize":"0.020000","newClientOid":"4"})",
      R"({"symbol":"BTCUSDT","productType":"USDT-FUTURES","marginCoin":"USDT","holdSide":"long","planType":"profit_loss","stopLossTriggerPrice":"58000.1","stopLossTriggerType":"mark_price","stopSurplusTriggerPrice":"65000.7","stopSurplusTriggerType":"mark_price","clientOid":"5"})",
      R"({"orderId":"TP-1","symbol":"BTCUSDT","productType":"USDT-FUTURES","marginCoin":"USDT","size":"1","triggerPrice":"58100.2","triggerType":"mark_price"})",
  };
  EXPECT_EQ(bitgetBodies(std::nullopt), expected);
  EXPECT_EQ(bitgetBodies(0.1), expected);
}

TEST(OnTickRequestPins, Hyperliquid)
{
  const std::vector<std::string> expected = {
      R"({"action":{"type":"order","orders":[{"a":0,"b":true,"p":"60000","s":"0.012","r":false,"t":{"limit":{"tif":"Gtc"}},"c":"0xX"}],"grouping":"na"},"nonce":X,"signature":{"r":"0xr","s":"0xs","v":27}})",
      R"({"action":{"type":"order","orders":[{"a":0,"b":false,"p":"101234","s":"0.5","r":false,"t":{"limit":{"tif":"Gtc"}},"c":"0xX"}],"grouping":"na"},"nonce":X,"signature":{"r":"0xr","s":"0xs","v":27}})",
      R"({"action":{"type":"order","orders":[{"a":0,"b":true,"p":"63000","s":"0.01","r":false,"t":{"limit":{"tif":"Ioc"}},"c":"0xX"}],"grouping":"na"},"nonce":X,"signature":{"r":"0xr","s":"0xs","v":27}})",
      R"({"action":{"type":"order","orders":[{"a":0,"b":false,"p":"52200","s":"1","r":false,"t":{"trigger":{"isMarket":true,"triggerPx":"58000","tpsl":"sl"}},"c":"0xX"}],"grouping":"na"},"nonce":X,"signature":{"r":"0xr","s":"0xs","v":27}})",
      R"({"action":{"type":"order","orders":[{"a":0,"b":false,"p":"57990","s":"1","r":false,"t":{"trigger":{"isMarket":false,"triggerPx":"58001","tpsl":"sl"}},"c":"0xX"}],"grouping":"na"},"nonce":X,"signature":{"r":"0xr","s":"0xs","v":27}})",
      R"({"action":{"type":"order","orders":[{"a":0,"b":false,"p":"65010","s":"1","r":false,"t":{"trigger":{"isMarket":false,"triggerPx":"65000","tpsl":"tp"}},"c":"0xX"}],"grouping":"na"},"nonce":X,"signature":{"r":"0xr","s":"0xs","v":27}})",
      R"({"action":{"type":"order","orders":[{"a":1,"b":true,"p":"0.12345","s":"100","r":false,"t":{"limit":{"tif":"Gtc"}},"c":"0xX"}],"grouping":"na"},"nonce":X,"signature":{"r":"0xr","s":"0xs","v":27}})",
      R"({"action":{"type":"modify","oid":777,"order":{"a":0,"b":true,"p":"60001","s":"0.02","r":false,"t":{"limit":{"tif":"Gtc"}},"c":"0xX"}},"nonce":X,"signature":{"r":"0xr","s":"0xs","v":27}})",
  };
  std::vector<std::string> unknown = hyperliquidBodies(std::nullopt);
  std::vector<std::string> known = hyperliquidBodies(0.1);
  for (auto& b : unknown)
  {
    b = maskNonce(b);
  }
  for (auto& b : known)
  {
    b = maskNonce(b);
  }
  EXPECT_EQ(unknown, expected);
  EXPECT_EQ(known, expected);
}
