/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Offline order-serialization tests for the Hyperliquid executor,
 * covering reduce-only and time-in-force serialization. Two fakes are
 * needed, for two different reasons:
 *
 *  - The transport is injected through the (new, test-only) constructor
 *    overload that takes a std::unique_ptr<ITransport> directly -- the
 *    same dependency-injection point Bybit/Bitget already had via
 *    AuthenticatedRestClient(..., ITransport*).
 *
 *  - hl_sign_with_sdk() is a free function, not virtual, called directly
 *    by the executor to sign every action before it will call
 *    transport->post() at all. It is defined here to shadow the real one:
 *    this translation unit's definition satisfies the linker before
 *    liblrvx-connectors.a's hl_signer.cpp.o is ever searched, so the real
 *    signer (which shells out to a live signing helper) never links in.
 *    This is the same link-seam technique the original audit repro used.
 */

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
std::optional<HlSig> hl_sign_with_sdk(const HlSignParams&)
{
  return HlSig{"0xdeadbeef", "0xcafebabe", 27};
}
}  // namespace lrvx::hl

using namespace lrvx;

namespace
{

std::string tempLogDir()
{
  auto dir = std::filesystem::temp_directory_path() / "lrvx_hl_order_test_logs";
  std::filesystem::create_directories(dir);
  return dir.string();
}

struct Call
{
  std::string url;
  std::string body;
};

// The executor's constructor fetches the asset-id map (loadAssetIds()) from
// the /info URL derived from the restUrl: the /info next to an /exchange
// URL, or the mainnet /info for any other URL, such as the ones these tests
// pass. Every order call goes to the restUrl itself. The fake tells the two
// apart by the /info suffix so the meta round-trip (needed to make
// assetIdFor() resolve "BTC") is not counted or asserted on as an order call.
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
        onSuccess(R"({"universe":[{"name":"BTC"}]})");
      }
      return;
    }
    calls.push_back({std::string(url), std::string(body)});
    if (onSuccess)
    {
      onSuccess(nextResponse);
    }
  }

  std::vector<Call> calls;  // order/cancel/replace calls only, not the meta fetch
  std::string nextResponse =
      R"({"status":"ok","response":{"type":"order","data":{"statuses":[{"resting":{"oid":777}}]}}})";
};

std::shared_ptr<AtomicLogger> makeLogger(const char* name)
{
  AtomicLoggerOptions logOpts;
  logOpts.directory = tempLogDir();
  logOpts.basename = name;
  return std::make_shared<AtomicLogger>(logOpts);
}

}  // namespace

// The reduce-only flag must reach the exchange. Scenario:
// +1.0 BTC position, a stop already trimmed 0.6, the strategy
// asks to close 1.0 reduce-only. If "r" is hardcoded false, HL executes
// the full 1.0 against a +0.4 remaining position -> a 0.6 short opens
// under full leverage instead of flattening.
TEST(HyperliquidOrderSerialization, ReduceOnlyFlagIsSerialized)
{
  auto transport = std::make_unique<FakeTransport>();
  auto* raw = transport.get();

  SymbolRegistry registry;
  OrderTracker tracker;
  SymbolInfo info;
  info.exchange = "hyperliquid";
  info.symbol = "BTC";
  info.type = InstrumentType::Future;
  SymbolId sym = registry.registerSymbol(info);

  HyperliquidOrderExecutorT<NoPolicies> executor(std::move(transport), "https://unused.invalid",
                                                 "aa", &registry, &tracker, makeLogger("hl_ro.log"),
                                                 "0xacct", std::nullopt, true);

  Order order;
  order.id = 1;
  order.symbol = sym;
  order.side = Side::SELL;
  order.type = OrderType::MARKET;
  order.price = Price::fromDouble(60000.0);
  order.quantity = Quantity::fromDouble(1.0);
  order.flags.reduceOnly = 1;
  order.timeInForce = TimeInForce::IOC;

  executor.submitOrder(order);

  ASSERT_EQ(raw->calls.size(), 1u);
  EXPECT_NE(raw->calls[0].body.find(R"("r":true)"), std::string::npos)
      << "reduceOnly must serialize as \"r\":true, not a hardcoded false: " << raw->calls[0].body;
}

TEST(HyperliquidOrderSerialization, ReduceOnlyFalseAlsoSerializesExplicitly)
{
  auto transport = std::make_unique<FakeTransport>();
  auto* raw = transport.get();

  SymbolRegistry registry;
  OrderTracker tracker;
  SymbolInfo info;
  info.exchange = "hyperliquid";
  info.symbol = "BTC";
  info.type = InstrumentType::Future;
  SymbolId sym = registry.registerSymbol(info);

  HyperliquidOrderExecutorT<NoPolicies> executor(
      std::move(transport), "https://unused.invalid", "aa", &registry, &tracker,
      makeLogger("hl_ro2.log"), "0xacct", std::nullopt, true);

  Order order;
  order.id = 2;
  order.symbol = sym;
  order.side = Side::BUY;
  order.type = OrderType::LIMIT;
  order.price = Price::fromDouble(60000.0);
  order.quantity = Quantity::fromDouble(1.0);
  order.flags.reduceOnly = 0;

  executor.submitOrder(order);

  ASSERT_EQ(raw->calls.size(), 1u);
  EXPECT_NE(raw->calls[0].body.find(R"("r":false)"), std::string::npos) << raw->calls[0].body;
}

// Time-in-force must reach the exchange too, not a hardcoded Gtc
// regardless of what the order asked for.
TEST(HyperliquidOrderSerialization, IocTimeInForceIsSerialized)
{
  auto transport = std::make_unique<FakeTransport>();
  auto* raw = transport.get();

  SymbolRegistry registry;
  OrderTracker tracker;
  SymbolInfo info;
  info.exchange = "hyperliquid";
  info.symbol = "BTC";
  info.type = InstrumentType::Future;
  SymbolId sym = registry.registerSymbol(info);

  HyperliquidOrderExecutorT<NoPolicies> executor(
      std::move(transport), "https://unused.invalid", "aa", &registry, &tracker,
      makeLogger("hl_tif.log"), "0xacct", std::nullopt, true);

  Order order;
  order.id = 3;
  order.symbol = sym;
  order.side = Side::BUY;
  order.type = OrderType::LIMIT;
  order.price = Price::fromDouble(60000.0);
  order.quantity = Quantity::fromDouble(1.0);
  order.timeInForce = TimeInForce::IOC;

  executor.submitOrder(order);

  ASSERT_EQ(raw->calls.size(), 1u);
  EXPECT_NE(raw->calls[0].body.find(R"("tif":"Ioc")"), std::string::npos) << raw->calls[0].body;
}

TEST(HyperliquidOrderSerialization, PostOnlyMapsToAlo)
{
  auto transport = std::make_unique<FakeTransport>();
  auto* raw = transport.get();

  SymbolRegistry registry;
  OrderTracker tracker;
  SymbolInfo info;
  info.exchange = "hyperliquid";
  info.symbol = "BTC";
  info.type = InstrumentType::Future;
  SymbolId sym = registry.registerSymbol(info);

  HyperliquidOrderExecutorT<NoPolicies> executor(std::move(transport), "https://unused.invalid",
                                                 "aa", &registry, &tracker, makeLogger("hl_po.log"),
                                                 "0xacct", std::nullopt, true);

  Order order;
  order.id = 4;
  order.symbol = sym;
  order.side = Side::BUY;
  order.type = OrderType::LIMIT;
  order.price = Price::fromDouble(60000.0);
  order.quantity = Quantity::fromDouble(1.0);
  order.flags.postOnly = 1;

  executor.submitOrder(order);

  ASSERT_EQ(raw->calls.size(), 1u);
  EXPECT_NE(raw->calls[0].body.find(R"("tif":"Alo")"), std::string::npos) << raw->calls[0].body;
}

// A market order has no distinct HL primitive; the standard convention
// (also used by HL's own SDK) is an IOC limit at the caller-supplied
// (slippage-adjusted) price, not a resting Gtc order.
TEST(HyperliquidOrderSerialization, MarketOrderUsesIocRegardlessOfRequestedTif)
{
  auto transport = std::make_unique<FakeTransport>();
  auto* raw = transport.get();

  SymbolRegistry registry;
  OrderTracker tracker;
  SymbolInfo info;
  info.exchange = "hyperliquid";
  info.symbol = "BTC";
  info.type = InstrumentType::Future;
  SymbolId sym = registry.registerSymbol(info);

  HyperliquidOrderExecutorT<NoPolicies> executor(
      std::move(transport), "https://unused.invalid", "aa", &registry, &tracker,
      makeLogger("hl_market.log"), "0xacct", std::nullopt, true);

  Order order;
  order.id = 5;
  order.symbol = sym;
  order.side = Side::SELL;
  order.type = OrderType::MARKET;
  order.price = Price::fromDouble(59000.0);
  order.quantity = Quantity::fromDouble(1.0);
  order.timeInForce = TimeInForce::GTC;  // caller's tif is irrelevant for a market order

  executor.submitOrder(order);

  ASSERT_EQ(raw->calls.size(), 1u);
  EXPECT_NE(raw->calls[0].body.find(R"("tif":"Ioc")"), std::string::npos) << raw->calls[0].body;
}

// A stop/take-profit/trailing order has no implementation in this
// connector (it only ever builds the "limit" order-type object). Before
// the fix, such an order silently went out as a plain resting Gtc limit
// with the trigger price and trailing rate dropped entirely -- the
// strategy believed a stop existed when nothing protective was on the
// exchange. The fix rejects it instead.
TEST(HyperliquidOrderSerialization, UnsupportedOrderTypesAreRejectedNotSilentlyDowngraded)
{
  auto transport = std::make_unique<FakeTransport>();
  auto* raw = transport.get();

  SymbolRegistry registry;
  OrderTracker tracker;
  SymbolInfo info;
  info.exchange = "hyperliquid";
  info.symbol = "BTC";
  info.type = InstrumentType::Future;
  SymbolId sym = registry.registerSymbol(info);

  HyperliquidOrderExecutorT<NoPolicies> executor(
      std::move(transport), "https://unused.invalid", "aa", &registry, &tracker,
      makeLogger("hl_unsup.log"), "0xacct", std::nullopt, true);

  Order order;
  order.id = 6;
  order.symbol = sym;
  order.side = Side::SELL;
  order.type = OrderType::TRAILING_STOP;
  order.price = Price::fromDouble(59000.0);
  order.quantity = Quantity::fromDouble(1.0);
  order.trailingCallbackRate = 100;

  executor.submitOrder(order);

  EXPECT_EQ(raw->calls.size(), 0u)
      << "an unimplemented order type must never reach the exchange as a fake resting limit";
  EXPECT_FALSE(tracker.exists(6));
}
