/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Offline tests for the Hyperliquid executor's trigger orders, position
 * TP/SL, leverage and price/size precision. Same two seams as
 * unit_test_order_serialization_hyperliquid.cpp: a fake ITransport injected
 * through the test constructor records every request body, and
 * hl_sign_with_sdk is shadowed at link time so no signing helper runs. The
 * shadow also records the action and the active pool it was asked to sign, so
 * each test can check that the body carries exactly what was signed.
 */

#include "lrvx-connectors/hyperliquid/hl_signer.h"
#include "lrvx-connectors/hyperliquid/hyperliquid_order_executor.h"

#include <lrvx/common.h>
#include <lrvx/engine/symbol_registry.h>
#include <lrvx/execution/abstract_execution_listener.h>
#include <lrvx/execution/bus/order_execution_bus.h>
#include <lrvx/execution/events/order_event.h>
#include <lrvx/execution/order.h>
#include <lrvx/execution/order_tracker.h>
#include <lrvx/log/atomic_logger.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <string>
#include <thread>
#include <vector>

namespace
{
std::vector<std::string> gSignedActions;
std::vector<std::optional<std::string>> gSignedActivePools;
}  // namespace

namespace lrvx::hl
{
std::optional<HlSig> hl_sign_with_sdk(const HlSignParams& p)
{
  gSignedActions.push_back(p.actionJson);
  gSignedActivePools.push_back(p.activePoolJson);
  return HlSig{"0xdeadbeef", "0xcafebabe", 27};
}
}  // namespace lrvx::hl

using namespace lrvx;

namespace
{

std::string tempLogDir()
{
  auto dir = std::filesystem::temp_directory_path() / "lrvx_hl_trigger_test_logs";
  std::filesystem::create_directories(dir);
  return dir.string();
}

std::shared_ptr<AtomicLogger> makeLogger(const char* name)
{
  AtomicLoggerOptions opts;
  opts.directory = tempLogDir();
  opts.basename = name;
  return std::make_shared<AtomicLogger>(opts);
}

// Asset ids follow the order of "universe": BTC = 0, ETH = 1, PEPE = 2.
// szDecimals are HL's real ones for BTC and ETH; PEPE stands in for a coin
// with no size decimals and a sub-cent price.
constexpr const char* kMeta =
    R"({"universe":[{"name":"BTC","szDecimals":5},{"name":"ETH","szDecimals":4},)"
    R"({"name":"PEPE","szDecimals":0}]})";

class FakeTransport final : public ITransport
{
 public:
  void post(std::string_view url, std::string_view body,
            const std::vector<std::pair<std::string_view, std::string_view>>&,
            MoveOnlyFunction<void(std::string_view)> onSuccess,
            MoveOnlyFunction<void(std::string_view)> onError) override
  {
    if (url.ends_with("/info"))
    {
      infoUrls.push_back(std::string(url));
      if (holdMeta)
      {
        heldMeta.push_back(std::move(onSuccess));
        return;
      }
      if (metaFailuresLeft > 0)
      {
        --metaFailuresLeft;
        if (onError)
        {
          onError("connect timeout");
        }
        return;
      }
      if (onSuccess)
      {
        onSuccess(kMeta);
      }
      return;
    }
    calls.push_back(std::string(body));
    if (!exchangeError.empty())
    {
      const std::string err = std::move(exchangeError);
      exchangeError.clear();
      if (onError)
      {
        onError(err);
      }
      return;
    }
    std::string resp = responses.empty() ? defaultResponse : responses.front();
    if (!responses.empty())
    {
      responses.erase(responses.begin());
    }
    if (onSuccess)
    {
      onSuccess(resp);
    }
  }

  std::vector<std::string> calls;
  std::vector<std::string> infoUrls;  // every meta request, by URL
  int metaFailuresLeft{0};            // meta requests to fail before answering
  bool holdMeta{false};               // keep meta requests unanswered until deliverMeta()
  std::vector<MoveOnlyFunction<void(std::string_view)>> heldMeta;
  std::string exchangeError;  // non-empty: the next /exchange request fails with it

  void deliverMeta()
  {
    auto held = std::move(heldMeta);
    heldMeta.clear();
    for (auto& onSuccess : held)
    {
      if (onSuccess)
      {
        onSuccess(kMeta);
      }
    }
  }
  std::vector<std::string> responses;  // consumed in order, then defaultResponse
  std::string defaultResponse =
      R"({"status":"ok","response":{"type":"order","data":{"statuses":[{"resting":{"oid":777}}]}}})";
};

class EventLog final : public IOrderExecutionListener
{
 public:
  EventLog() : IOrderExecutionListener(21) {}

  void onOrderEvent(const OrderEvent& ev) override
  {
    std::lock_guard<std::mutex> lk(_m);
    _events.push_back(ev);
  }

  std::vector<OrderEvent> events()
  {
    std::lock_guard<std::mutex> lk(_m);
    return _events;
  }

 private:
  std::mutex _m;
  std::vector<OrderEvent> _events;
};

struct Harness
{
  std::unique_ptr<FakeTransport> owner{std::make_unique<FakeTransport>()};
  FakeTransport* transport{owner.get()};
  SymbolRegistry registry;
  OrderTracker tracker;
  SymbolId btc{0};
  SymbolId eth{0};
  SymbolId pepe{0};
  OrderExecutionBus bus;
  EventLog log;
  std::unique_ptr<HyperliquidOrderExecutor> executor;

  explicit Harness(const char* logName, std::string restUrl = "https://unused.invalid/exchange",
                   int metaFailures = 0, std::optional<std::string> vaultAddress = std::nullopt)
  {
    gSignedActions.clear();
    gSignedActivePools.clear();
    transport->metaFailuresLeft = metaFailures;
    auto reg = [&](const char* name)
    {
      SymbolInfo info;
      info.exchange = "hyperliquid";
      info.symbol = name;
      info.type = InstrumentType::Future;
      return registry.registerSymbol(info);
    };
    btc = reg("BTC");
    eth = reg("ETH");
    pepe = reg("PEPE");

    executor = std::make_unique<HyperliquidOrderExecutor>(
        std::move(owner), std::move(restUrl), "aa", &registry, &tracker, makeLogger(logName),
        "0xacct", std::move(vaultAddress), true);
    bus.subscribe(&log);
    bus.start();
    executor->setOrderBus(&bus);
  }

  ~Harness() { bus.stop(); }

  std::vector<OrderEvent> events()
  {
    bus.flush();
    return log.events();
  }
};

// The "action" object of a recorded /exchange body, with every 128-bit cloid
// replaced by CLOID so the expected JSON can be written out literally.
std::string actionOf(const std::string& body)
{
  const std::string head = "{\"action\":";
  const auto end = body.find(",\"nonce\":");
  if (body.rfind(head, 0) != 0 || end == std::string::npos)
  {
    return "<malformed body: " + body + ">";
  }
  return body.substr(head.size(), end - head.size());
}

std::string maskCloids(const std::string& s)
{
  static const std::regex cloid(R"("c":"0x[0-9a-f]{32}")");
  return std::regex_replace(s, cloid, R"("c":"CLOID")");
}

std::vector<std::string> cloidsIn(const std::string& s)
{
  static const std::regex cloid(R"re("c":"(0x[0-9a-f]{32})")re");
  std::vector<std::string> out;
  for (std::sregex_iterator it(s.begin(), s.end(), cloid), e; it != e; ++it)
  {
    out.push_back((*it)[1].str());
  }
  return out;
}

Order stopMarketSell(SymbolId sym)
{
  Order o;
  o.id = 10;
  o.symbol = sym;
  o.side = Side::SELL;
  o.type = OrderType::STOP_MARKET;
  o.triggerPrice = Price::fromDouble(58000.0);
  o.quantity = Quantity::fromDouble(0.5);
  o.flags.reduceOnly = 1;
  return o;
}

std::vector<OrderEventStatus> statusesFor(const std::vector<OrderEvent>& evs, OrderId id)
{
  std::vector<OrderEventStatus> out;
  for (const auto& e : evs)
  {
    if (e.order.id == id)
    {
      out.push_back(e.status);
    }
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------- trigger orders

TEST(HyperliquidTriggerOrders, StopMarketSerializesTriggerObjectWithSlippageLimit)
{
  Harness h("hl_trig_sm.log");
  h.executor->submitOrder(stopMarketSell(h.btc));

  ASSERT_EQ(h.transport->calls.size(), 1u);
  // p = 58000 * (1 - 0.10): a sell stop needs its limit below the trigger.
  EXPECT_EQ(maskCloids(actionOf(h.transport->calls[0])),
            R"({"type":"order","orders":[{"a":0,"b":false,"p":"52200","s":"0.5","r":true,)"
            R"("t":{"trigger":{"isMarket":true,"triggerPx":"58000","tpsl":"sl"}},)"
            R"("c":"CLOID"}],"grouping":"na"})");
  ASSERT_EQ(gSignedActions.size(), 1u);
  EXPECT_EQ(gSignedActions[0], actionOf(h.transport->calls[0]))
      << "the action sent must be the action that was signed";
}

TEST(HyperliquidTriggerOrders, TakeProfitMarketBuyPutsSlippageAboveTrigger)
{
  Harness h("hl_trig_tpm.log");
  Order o;
  o.id = 11;
  o.symbol = h.btc;
  o.side = Side::BUY;
  o.type = OrderType::TAKE_PROFIT_MARKET;
  o.triggerPrice = Price::fromDouble(50000.0);
  o.price = Price::fromDouble(50000.0);  // ignored for a market trigger
  o.quantity = Quantity::fromDouble(0.25);
  o.flags.reduceOnly = 1;
  h.executor->submitOrder(o);

  ASSERT_EQ(h.transport->calls.size(), 1u);
  EXPECT_EQ(maskCloids(actionOf(h.transport->calls[0])),
            R"({"type":"order","orders":[{"a":0,"b":true,"p":"55000","s":"0.25","r":true,)"
            R"("t":{"trigger":{"isMarket":true,"triggerPx":"50000","tpsl":"tp"}},)"
            R"("c":"CLOID"}],"grouping":"na"})");
}

TEST(HyperliquidTriggerOrders, LimitTriggersCarryTheOrdersLimitPrice)
{
  Harness h("hl_trig_lim.log");
  Order sl;
  sl.id = 12;
  sl.symbol = h.btc;
  sl.side = Side::BUY;
  sl.type = OrderType::STOP_LIMIT;
  sl.triggerPrice = Price::fromDouble(61000.0);
  sl.price = Price::fromDouble(61500.0);
  sl.quantity = Quantity::fromDouble(1.0);
  h.executor->submitOrder(sl);

  Order tp = sl;
  tp.id = 13;
  tp.side = Side::SELL;
  tp.type = OrderType::TAKE_PROFIT_LIMIT;
  tp.triggerPrice = Price::fromDouble(65000.0);
  tp.price = Price::fromDouble(64900.0);
  h.executor->submitOrder(tp);

  ASSERT_EQ(h.transport->calls.size(), 2u);
  EXPECT_EQ(maskCloids(actionOf(h.transport->calls[0])),
            R"({"type":"order","orders":[{"a":0,"b":true,"p":"61500","s":"1","r":false,)"
            R"("t":{"trigger":{"isMarket":false,"triggerPx":"61000","tpsl":"sl"}},)"
            R"("c":"CLOID"}],"grouping":"na"})");
  EXPECT_EQ(maskCloids(actionOf(h.transport->calls[1])),
            R"({"type":"order","orders":[{"a":0,"b":false,"p":"64900","s":"1","r":false,)"
            R"("t":{"trigger":{"isMarket":false,"triggerPx":"65000","tpsl":"tp"}},)"
            R"("c":"CLOID"}],"grouping":"na"})");
}

TEST(HyperliquidTriggerOrders, RestingTriggerIsPendingTriggerAndCancellable)
{
  Harness h("hl_trig_rest.log");
  h.executor->submitOrder(stopMarketSell(h.btc));

  auto evs = h.events();
  EXPECT_EQ(statusesFor(evs, 10),
            (std::vector<OrderEventStatus>{OrderEventStatus::SUBMITTED,
                                           OrderEventStatus::PENDING_TRIGGER}));
  auto st = h.tracker.get(10);
  ASSERT_TRUE(st.has_value());
  EXPECT_EQ(st->exchangeOrderId, "777");
  ASSERT_EQ(h.transport->calls.size(), 1u);
  const auto cloids = cloidsIn(h.transport->calls[0]);
  ASSERT_EQ(cloids.size(), 1u);
  EXPECT_EQ(st->clientOrderId, cloids[0]);

  h.transport->responses.push_back(
      R"({"status":"ok","response":{"type":"cancel","data":{"statuses":["success"]}}})");
  h.executor->cancelOrder(10);
  ASSERT_EQ(h.transport->calls.size(), 2u);
  EXPECT_EQ(actionOf(h.transport->calls[1]),
            R"({"type":"cancelByCloid","cancels":[{"asset":0,"cloid":")" + cloids[0] + R"("}]})");
  EXPECT_FALSE(h.tracker.isActive(10));
}

TEST(HyperliquidTriggerOrders, TriggerWithoutTriggerPriceIsRejected)
{
  Harness h("hl_trig_notrig.log");
  Order o = stopMarketSell(h.btc);
  o.triggerPrice = Price{};
  h.executor->submitOrder(o);

  Order sl = stopMarketSell(h.btc);
  sl.id = 14;
  sl.type = OrderType::STOP_LIMIT;  // no limit price
  h.executor->submitOrder(sl);

  EXPECT_TRUE(h.transport->calls.empty());
  auto evs = h.events();
  EXPECT_EQ(statusesFor(evs, 10), (std::vector<OrderEventStatus>{OrderEventStatus::REJECTED}));
  EXPECT_EQ(statusesFor(evs, 14), (std::vector<OrderEventStatus>{OrderEventStatus::REJECTED}));
  EXPECT_FALSE(h.tracker.exists(10));
  EXPECT_FALSE(h.tracker.exists(14));
}

TEST(HyperliquidTriggerOrders, TrailingAndIcebergAreRejectedOnTheBus)
{
  Harness h("hl_trig_trail.log");
  Order o;
  o.id = 15;
  o.symbol = h.btc;
  o.side = Side::SELL;
  o.type = OrderType::TRAILING_STOP;
  o.price = Price::fromDouble(59000.0);
  o.quantity = Quantity::fromDouble(1.0);
  o.trailingCallbackRate = 100;
  h.executor->submitOrder(o);

  Order ice = o;
  ice.id = 16;
  ice.type = OrderType::ICEBERG;
  h.executor->submitOrder(ice);

  EXPECT_TRUE(h.transport->calls.empty());
  auto evs = h.events();
  EXPECT_EQ(statusesFor(evs, 15), (std::vector<OrderEventStatus>{OrderEventStatus::REJECTED}));
  EXPECT_EQ(statusesFor(evs, 16), (std::vector<OrderEventStatus>{OrderEventStatus::REJECTED}));
}

TEST(HyperliquidTriggerOrders, VenueErrorOnTriggerIsRejected)
{
  Harness h("hl_trig_err.log");
  h.transport->responses.push_back(
      R"({"status":"ok","response":{"type":"order","data":{"statuses":[)"
      R"({"error":"Reduce only order would increase position."}]}}})");
  h.executor->submitOrder(stopMarketSell(h.btc));

  auto evs = h.events();
  ASSERT_EQ(statusesFor(evs, 10), (std::vector<OrderEventStatus>{OrderEventStatus::REJECTED}));
  EXPECT_EQ(evs[0].rejectReason, "Reduce only order would increase position.");
  EXPECT_FALSE(h.tracker.exists(10));
}

// ---------------------------------------------------------------- position TP/SL

TEST(HyperliquidPositionTpsl, BothLegsGoOutInOneActionGroupedAsPositionTpsl)
{
  Harness h("hl_pos_both.log");
  h.transport->responses.push_back(
      R"({"status":"ok","response":{"type":"order","data":{"statuses":[)"
      R"({"resting":{"oid":101}},{"resting":{"oid":102}}]}}})");

  h.executor->placePosTpsl(h.btc, Side::BUY, Price::fromDouble(58000.0), Price::fromDouble(65000.0),
                           Quantity::fromDouble(0.5), 20, 21);

  ASSERT_EQ(h.transport->calls.size(), 1u) << "one action, not one request per leg";
  EXPECT_EQ(maskCloids(actionOf(h.transport->calls[0])),
            R"({"type":"order","orders":[)"
            R"({"a":0,"b":false,"p":"52200","s":"0.5","r":true,)"
            R"("t":{"trigger":{"isMarket":true,"triggerPx":"58000","tpsl":"sl"}},"c":"CLOID"},)"
            R"({"a":0,"b":false,"p":"58500","s":"0.5","r":true,)"
            R"("t":{"trigger":{"isMarket":true,"triggerPx":"65000","tpsl":"tp"}},"c":"CLOID"}],)"
            R"("grouping":"positionTpsl"})");
  ASSERT_EQ(gSignedActions.size(), 1u);
  EXPECT_EQ(gSignedActions[0], actionOf(h.transport->calls[0]));

  auto evs = h.events();
  const std::vector<OrderEventStatus> pending{OrderEventStatus::SUBMITTED,
                                              OrderEventStatus::PENDING_TRIGGER};
  EXPECT_EQ(statusesFor(evs, 20), pending);
  EXPECT_EQ(statusesFor(evs, 21), pending);
  ASSERT_TRUE(h.tracker.get(20).has_value());
  ASSERT_TRUE(h.tracker.get(21).has_value());
  EXPECT_EQ(h.tracker.get(20)->exchangeOrderId, "101");
  EXPECT_EQ(h.tracker.get(21)->exchangeOrderId, "102");
  EXPECT_EQ(h.tracker.get(20)->localOrder.type, OrderType::STOP_MARKET);
  EXPECT_EQ(h.tracker.get(21)->localOrder.type, OrderType::TAKE_PROFIT_MARKET);
}

TEST(HyperliquidPositionTpsl, ShortPositionLegsBuyAndZeroTriggerSkipsALeg)
{
  Harness h("hl_pos_one.log");
  h.executor->placePosTpsl(h.btc, Side::SELL, Price::fromDouble(62000.0), Price{},
                           Quantity::fromDouble(0.5), 22, 23);

  ASSERT_EQ(h.transport->calls.size(), 1u);
  // A short's stop buys, with the slippage room above the trigger.
  EXPECT_EQ(maskCloids(actionOf(h.transport->calls[0])),
            R"({"type":"order","orders":[)"
            R"({"a":0,"b":true,"p":"68200","s":"0.5","r":true,)"
            R"("t":{"trigger":{"isMarket":true,"triggerPx":"62000","tpsl":"sl"}},"c":"CLOID"}],)"
            R"("grouping":"positionTpsl"})");
  auto evs = h.events();
  EXPECT_FALSE(statusesFor(evs, 22).empty());
  EXPECT_TRUE(statusesFor(evs, 23).empty()) << "the skipped leg must not appear at all";
}

TEST(HyperliquidPositionTpsl, BareWaitingStatusesAndPerLegErrors)
{
  Harness h("hl_pos_bare.log");
  h.transport->responses.push_back(
      R"({"status":"ok","response":{"type":"order","data":{"statuses":[)"
      R"("waitingForTrigger",{"error":"Invalid TP/SL price."}]}}})");
  h.executor->placePosTpsl(h.btc, Side::BUY, Price::fromDouble(58000.0), Price::fromDouble(65000.0),
                           Quantity::fromDouble(0.5), 24, 25);

  auto evs = h.events();
  EXPECT_EQ(statusesFor(evs, 24),
            (std::vector<OrderEventStatus>{OrderEventStatus::SUBMITTED,
                                           OrderEventStatus::PENDING_TRIGGER}));
  EXPECT_EQ(statusesFor(evs, 25), (std::vector<OrderEventStatus>{OrderEventStatus::REJECTED}));
  ASSERT_TRUE(h.tracker.get(24).has_value());
  EXPECT_FALSE(h.tracker.get(24)->clientOrderId.empty()) << "cancellable by cloid";
  EXPECT_FALSE(h.tracker.exists(25));
}

TEST(HyperliquidPositionTpsl, TopLevelErrorRejectsBothLegs)
{
  Harness h("hl_pos_err.log");
  h.transport->responses.push_back(R"({"status":"err","response":"Insufficient margin"})");
  h.executor->placePosTpsl(h.btc, Side::BUY, Price::fromDouble(58000.0), Price::fromDouble(65000.0),
                           Quantity::fromDouble(0.5), 26, 27);
  auto evs = h.events();
  EXPECT_EQ(statusesFor(evs, 26), (std::vector<OrderEventStatus>{OrderEventStatus::REJECTED}));
  EXPECT_EQ(statusesFor(evs, 27), (std::vector<OrderEventStatus>{OrderEventStatus::REJECTED}));
}

TEST(HyperliquidPositionTpsl, InvalidBracketsNeverReachTheVenue)
{
  Harness h("hl_pos_bad.log");
  // Long with the stop above the take-profit.
  h.executor->placePosTpsl(h.btc, Side::BUY, Price::fromDouble(66000.0), Price::fromDouble(65000.0),
                           Quantity::fromDouble(0.5), 30, 31);
  // No size.
  h.executor->placePosTpsl(h.btc, Side::BUY, Price::fromDouble(58000.0), Price{}, Quantity{}, 32,
                           33);
  // No local id for a leg that is sent.
  h.executor->placePosTpsl(h.btc, Side::BUY, Price::fromDouble(58000.0), Price{},
                           Quantity::fromDouble(0.5), 0, 0);
  // Nothing to send.
  h.executor->placePosTpsl(h.btc, Side::BUY, Price{}, Price{}, Quantity::fromDouble(0.5), 34, 35);

  EXPECT_TRUE(h.transport->calls.empty());
  auto evs = h.events();
  EXPECT_EQ(statusesFor(evs, 30), (std::vector<OrderEventStatus>{OrderEventStatus::REJECTED}));
  EXPECT_EQ(statusesFor(evs, 31), (std::vector<OrderEventStatus>{OrderEventStatus::REJECTED}));
  EXPECT_EQ(statusesFor(evs, 32), (std::vector<OrderEventStatus>{OrderEventStatus::REJECTED}));
  EXPECT_EQ(statusesFor(evs, 0), (std::vector<OrderEventStatus>{OrderEventStatus::REJECTED}))
      << "the leg without a local id is rejected, not sent";
}

// ---------------------------------------------------------------- leverage

TEST(HyperliquidLeverage, UpdateLeverageBodyAndSuccess)
{
  Harness h("hl_lev_ok.log");
  h.transport->responses.push_back(R"({"status":"ok","response":{"type":"default"}})");

  std::optional<bool> ok;
  std::string detail;
  h.executor->setLeverage(h.eth, 5, true,
                          [&](bool o, const std::string& d)
                          {
                            ok = o;
                            detail = d;
                          });

  ASSERT_EQ(h.transport->calls.size(), 1u);
  EXPECT_EQ(actionOf(h.transport->calls[0]),
            R"({"type":"updateLeverage","asset":1,"isCross":true,"leverage":5})");
  ASSERT_EQ(gSignedActions.size(), 1u);
  EXPECT_EQ(gSignedActions[0], actionOf(h.transport->calls[0]));
  ASSERT_TRUE(ok.has_value());
  EXPECT_TRUE(*ok);
}

TEST(HyperliquidLeverage, IsolatedAndVenueRefusalIsReported)
{
  Harness h("hl_lev_err.log");
  h.transport->responses.push_back(
      R"({"status":"err","response":"Cannot switch leverage type with open position."})");

  std::optional<bool> ok;
  std::string detail;
  h.executor->setLeverage(h.btc, 20, false,
                          [&](bool o, const std::string& d)
                          {
                            ok = o;
                            detail = d;
                          });

  ASSERT_EQ(h.transport->calls.size(), 1u);
  EXPECT_EQ(actionOf(h.transport->calls[0]),
            R"({"type":"updateLeverage","asset":0,"isCross":false,"leverage":20})");
  ASSERT_TRUE(ok.has_value());
  EXPECT_FALSE(*ok);
  EXPECT_EQ(detail, "Cannot switch leverage type with open position.");
}

TEST(HyperliquidLeverage, InvalidLeverageNeverLeavesTheProcess)
{
  Harness h("hl_lev_bad.log");
  std::optional<bool> ok;
  h.executor->setLeverage(h.btc, 0, true,
                          [&](bool o, const std::string&)
                          {
                            ok = o;
                          });
  EXPECT_TRUE(h.transport->calls.empty());
  ASSERT_TRUE(ok.has_value());
  EXPECT_FALSE(*ok);
}

// ---------------------------------------------------------------- market price

TEST(HyperliquidMarketPrice, SlippageGoesOnTheAggressiveSide)
{
  EXPECT_EQ(marketPriceWithSlippage(Price::fromDouble(60000.0), Side::BUY, 0.05),
            Price::fromDouble(63000.0));
  EXPECT_EQ(marketPriceWithSlippage(Price::fromDouble(60000.0), Side::SELL, 0.05),
            Price::fromDouble(57000.0));
  // 1.234567 * 1.01 = 1.24691267 -> 5 significant figures.
  EXPECT_EQ(marketPriceWithSlippage(Price::fromDouble(1.234567), Side::BUY, 0.01),
            Price::fromDouble(1.2469));
  // From 100000 up an integer price is always valid on HL.
  EXPECT_EQ(marketPriceWithSlippage(Price::fromDouble(123456.7), Side::BUY, 0.0),
            Price::fromDouble(123457.0));
  EXPECT_EQ(marketPriceWithSlippage(Price{}, Side::BUY, 0.05).raw(), 0);
  EXPECT_EQ(marketPriceWithSlippage(Price::fromDouble(100.0), Side::BUY, -0.1).raw(), 0);
  EXPECT_EQ(marketPriceWithSlippage(Price::fromDouble(100.0), Side::SELL, 1.0).raw(), 0);
}

TEST(HyperliquidMarketPrice, UnpricedMarketOrderIsRejectedNotSentAtZero)
{
  Harness h("hl_mkt_zero.log");
  Order o;
  o.id = 40;
  o.symbol = h.btc;
  o.side = Side::BUY;
  o.type = OrderType::MARKET;
  o.quantity = Quantity::fromDouble(0.1);
  h.executor->submitOrder(o);

  EXPECT_TRUE(h.transport->calls.empty());
  auto evs = h.events();
  ASSERT_EQ(statusesFor(evs, 40), (std::vector<OrderEventStatus>{OrderEventStatus::REJECTED}));
  EXPECT_NE(evs[0].rejectReason.find("market order without a price"), std::string::npos)
      << evs[0].rejectReason;
}

// ---------------------------------------------------------------- precision

TEST(HyperliquidPrecision, PricesAndSizesFollowSzDecimalsAndFiveSigFigs)
{
  Harness h("hl_prec.log");
  Order eth;
  eth.id = 50;
  eth.symbol = h.eth;
  eth.side = Side::BUY;
  eth.type = OrderType::LIMIT;
  eth.price = Price::fromDouble(3456.789);        // 5 sig figs -> 3456.8
  eth.quantity = Quantity::fromDouble(0.123456);  // szDecimals 4, truncated
  h.executor->submitOrder(eth);

  Order pepe;
  pepe.id = 51;
  pepe.symbol = h.pepe;
  pepe.side = Side::SELL;
  pepe.type = OrderType::LIMIT;
  pepe.price = Price::fromDouble(0.0001234567);  // 6 - 0 = 6 decimals max
  pepe.quantity = Quantity::fromDouble(1000.9);  // szDecimals 0
  h.executor->submitOrder(pepe);

  ASSERT_EQ(h.transport->calls.size(), 2u);
  EXPECT_NE(h.transport->calls[0].find(R"("p":"3456.8","s":"0.1234")"), std::string::npos)
      << h.transport->calls[0];
  EXPECT_NE(h.transport->calls[1].find(R"("p":"0.000123","s":"1000")"), std::string::npos)
      << h.transport->calls[1];
}

TEST(HyperliquidPrecision, SizeBelowOneLotIsRejected)
{
  Harness h("hl_prec_zero.log");
  Order o;
  o.id = 52;
  o.symbol = h.btc;
  o.side = Side::BUY;
  o.type = OrderType::LIMIT;
  o.price = Price::fromDouble(60000.0);
  o.quantity = Quantity::fromDouble(0.000004);  // below 1e-5, BTC's szDecimals
  h.executor->submitOrder(o);

  EXPECT_TRUE(h.transport->calls.empty());
  auto evs = h.events();
  EXPECT_EQ(statusesFor(evs, 52), (std::vector<OrderEventStatus>{OrderEventStatus::REJECTED}));
}

// ---------------------------------------------------------------- nonces

namespace
{
uint64_t nonceOf(const std::string& body)
{
  static const std::regex nonce(R"re("nonce":(\d+))re");
  std::smatch m;
  if (!std::regex_search(body, m, nonce))
  {
    return 0;
  }
  return std::stoull(m[1].str());
}
}  // namespace

// Back-to-back actions land in the same millisecond, and the venue refuses a
// nonce it has already seen, so each one gets a nonce above the last.
TEST(HyperliquidNonces, EveryActionGetsAStrictlyIncreasingNonce)
{
  Harness h("hl_nonce.log");
  Order o;
  o.id = 60;
  o.symbol = h.btc;
  o.side = Side::BUY;
  o.type = OrderType::LIMIT;
  o.price = Price::fromDouble(60000.0);
  o.quantity = Quantity::fromDouble(0.01);
  h.executor->submitOrder(o);
  h.executor->cancelOrder(60);
  h.transport->responses.push_back(R"({"status":"ok","response":{"type":"default"}})");
  h.executor->setLeverage(h.btc, 5, true);
  h.executor->placePosTpsl(h.btc, Side::BUY, Price::fromDouble(58000.0), Price::fromDouble(62000.0),
                           Quantity::fromDouble(0.01), 61, 62);
  o.id = 63;
  h.executor->submitOrder(o);

  ASSERT_EQ(h.transport->calls.size(), 5u);
  uint64_t prev = 0;
  for (const auto& body : h.transport->calls)
  {
    const uint64_t n = nonceOf(body);
    ASSERT_GT(n, prev) << body;
    prev = n;
  }
}

// ---------------------------------------------------------------- asset map

namespace
{
Order limitBuy(SymbolId sym, OrderId id)
{
  Order o;
  o.id = id;
  o.symbol = sym;
  o.side = Side::BUY;
  o.type = OrderType::LIMIT;
  o.price = Price::fromDouble(60000.0);
  o.quantity = Quantity::fromDouble(0.01);
  return o;
}
}  // namespace

// A failed first meta request leaves the map empty. The next use starts a
// new request without waiting for it and is refused; further uses inside the
// retry interval send no request of their own; the answer loads the map for
// the uses that follow.
TEST(HyperliquidAssetMap, FailedFirstFetchIsRetriedWithoutBlockingAndThrottled)
{
  Harness h("hl_assets_retry.log", "https://unused.invalid/exchange", /*metaFailures=*/1);
  ASSERT_EQ(h.transport->infoUrls.size(), 1u);
  EXPECT_FALSE(h.executor->assetsLoaded());
  h.transport->holdMeta = true;

  // First use: starts the retry, does not wait for it, and is refused.
  const auto start = std::chrono::steady_clock::now();
  h.executor->submitOrder(limitBuy(h.btc, 70));
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(500))
      << "the retry must not hold the caller";
  EXPECT_EQ(h.transport->infoUrls.size(), 2u);
  EXPECT_EQ(h.transport->heldMeta.size(), 1u);
  EXPECT_TRUE(h.transport->calls.empty());

  // Inside the retry interval: refused without another request.
  h.executor->submitOrder(limitBuy(h.btc, 71));
  EXPECT_EQ(h.transport->infoUrls.size(), 2u) << "retry must be throttled to once per 2 s";
  EXPECT_TRUE(h.transport->calls.empty());

  // The answer arrives and loads the map; the next order goes out with the
  // right asset index and asks for nothing more.
  h.transport->deliverMeta();
  EXPECT_TRUE(h.executor->assetsLoaded());
  h.executor->submitOrder(limitBuy(h.eth, 72));
  EXPECT_EQ(h.transport->infoUrls.size(), 2u);
  ASSERT_EQ(h.transport->calls.size(), 1u);
  EXPECT_NE(h.transport->calls[0].find(R"("a":1,)"), std::string::npos) << h.transport->calls[0];

  auto evs = h.events();
  EXPECT_EQ(statusesFor(evs, 70), (std::vector<OrderEventStatus>{OrderEventStatus::REJECTED}));
  EXPECT_EQ(statusesFor(evs, 71), (std::vector<OrderEventStatus>{OrderEventStatus::REJECTED}));
  EXPECT_EQ(statusesFor(evs, 72), (std::vector<OrderEventStatus>{OrderEventStatus::SUBMITTED}));
  for (const auto& e : evs)
  {
    if (e.order.id == 70)
    {
      EXPECT_NE(e.rejectReason.find("asset map not loaded"), std::string::npos) << e.rejectReason;
    }
  }
}

TEST(HyperliquidAssetMap, LoadedOnFirstFetchReportsLoaded)
{
  Harness h("hl_assets_ok.log");
  EXPECT_TRUE(h.executor->assetsLoaded());
  EXPECT_EQ(h.transport->infoUrls.size(), 1u);
}

// Asset indices differ between networks, so the meta request goes to the
// /info endpoint next to the executor's rest URL.
TEST(HyperliquidAssetMap, InfoUrlIsDerivedFromTheRestUrl)
{
  Harness h("hl_assets_testnet.log", "https://api.hyperliquid-testnet.xyz/exchange");
  ASSERT_EQ(h.transport->infoUrls.size(), 1u);
  EXPECT_EQ(h.transport->infoUrls[0], "https://api.hyperliquid-testnet.xyz/info");
}

TEST(HyperliquidAssetMap, RestUrlWithoutExchangeSuffixFallsBackToMainnetInfo)
{
  Harness h("hl_assets_fallback.log", "http://127.0.0.1:9/custom");
  ASSERT_EQ(h.transport->infoUrls.size(), 1u);
  EXPECT_EQ(h.transport->infoUrls[0], "https://api.hyperliquid.xyz/info");
}

// ---------------------------------------------------------------- logging

namespace
{
class CaptureLogger final : public ILogger
{
 public:
  void info(std::string_view m) override { add(m); }
  void warn(std::string_view m) override { add(m); }
  void error(std::string_view m) override { add(m); }

  std::vector<std::string> lines()
  {
    std::lock_guard<std::mutex> lk(_m);
    return _lines;
  }

 private:
  void add(std::string_view m)
  {
    std::lock_guard<std::mutex> lk(_m);
    _lines.emplace_back(m);
  }
  std::mutex _m;
  std::vector<std::string> _lines;
};
}  // namespace

// One info line per action names the action type and the local ids; no line
// carries the signature.
TEST(HyperliquidLogging, ActionsAreLoggedWithoutTheirSignature)
{
  gSignedActions.clear();
  auto owner = std::make_unique<FakeTransport>();
  FakeTransport* transport = owner.get();
  SymbolRegistry registry;
  SymbolInfo si;
  si.exchange = "hyperliquid";
  si.symbol = "BTC";
  si.type = InstrumentType::Future;
  const SymbolId btc = registry.registerSymbol(si);
  OrderTracker tracker;
  auto logger = std::make_shared<CaptureLogger>();
  HyperliquidOrderExecutor executor(std::move(owner), "https://unused.invalid/exchange", "aa",
                                    &registry, &tracker, logger, "0xacct", std::nullopt, true);

  executor.submitOrder(limitBuy(btc, 80));
  Order moved = limitBuy(btc, 81);
  moved.price = Price::fromDouble(60100.0);
  executor.replaceOrder(80, moved);
  executor.cancelOrder(80);
  transport->responses.push_back(R"({"status":"ok","response":{"type":"default"}})");
  executor.setLeverage(btc, 3, true);
  executor.placePosTpsl(btc, Side::BUY, Price::fromDouble(58000.0), Price::fromDouble(62000.0),
                        Quantity::fromDouble(0.01), 82, 83);
  ASSERT_EQ(transport->calls.size(), 5u);

  const auto lines = logger->lines();
  for (const auto& line : lines)
  {
    EXPECT_EQ(line.find("0xdeadbeef"), std::string::npos) << "signature in the log: " << line;
    EXPECT_EQ(line.find("signature"), std::string::npos) << line;
  }
  auto has = [&](const std::string& needle)
  {
    return std::any_of(lines.begin(), lines.end(),
                       [&](const std::string& l)
                       {
                         return l.find(needle) != std::string::npos;
                       });
  };
  EXPECT_TRUE(has("[HL] order grouping=na localIds=80 "));
  EXPECT_TRUE(has("[HL] modify localId=80 newLocalId=81 "));
  EXPECT_TRUE(has("[HL] cancelByCloid localId=80 "));
  EXPECT_TRUE(has("[HL] updateLeverage BTC "));
  EXPECT_TRUE(has("[HL] order grouping=positionTpsl localIds=82,83 "));
}

// ---------------------------------------------------------------- vault address

namespace
{
constexpr const char* kVault = "0x1111111111111111111111111111111111111111";

std::string vaultFieldOf(const std::string& body)
{
  static const std::regex vault(R"re("vaultAddress":"([^"]*)")re");
  std::smatch m;
  if (!std::regex_search(body, m, vault))
  {
    return "";
  }
  return m[1].str();
}

// Submits a resting limit order (localId 90), modifies it into localId 91,
// then cancels 91. Returns the three /exchange bodies.
std::vector<std::string> submitModifyCancel(Harness& h)
{
  h.executor->submitOrder(limitBuy(h.btc, 90));
  Order moved = limitBuy(h.btc, 91);
  moved.price = Price::fromDouble(60100.0);
  h.transport->responses.push_back(R"({"status":"ok","response":{"type":"default"}})");
  h.executor->replaceOrder(90, moved);
  h.transport->responses.push_back(
      R"({"status":"ok","response":{"type":"cancel","data":{"statuses":["success"]}}})");
  h.executor->cancelOrder(91);
  return h.transport->calls;
}
}  // namespace

TEST(HyperliquidVaultAddress, CancelAndModifySignAndCarryTheVault)
{
  Harness h("hl_vault_on.log", "https://unused.invalid/exchange", 0, std::string(kVault));
  const auto calls = submitModifyCancel(h);

  ASSERT_EQ(calls.size(), 3u);
  EXPECT_NE(actionOf(calls[1]).find(R"("type":"modify")"), std::string::npos) << calls[1];
  EXPECT_NE(actionOf(calls[2]).find(R"("type":"cancelByCloid")"), std::string::npos) << calls[2];
  for (const auto& body : calls)
  {
    EXPECT_EQ(vaultFieldOf(body), kVault) << body;
  }

  ASSERT_EQ(gSignedActivePools.size(), 3u);
  ASSERT_EQ(gSignedActions.size(), 3u);
  for (size_t i = 0; i < 3; ++i)
  {
    ASSERT_TRUE(gSignedActivePools[i].has_value()) << "action " << i;
    // The signer request takes the active pool as a JSON value.
    EXPECT_EQ(*gSignedActivePools[i], std::string("\"") + kVault + "\"");
    EXPECT_EQ(gSignedActions[i], actionOf(calls[i]));
  }
}

TEST(HyperliquidVaultAddress, WithoutAVaultNeitherBodyNorSignatureNamesOne)
{
  Harness h("hl_vault_off.log");
  const auto calls = submitModifyCancel(h);

  ASSERT_EQ(calls.size(), 3u);
  for (const auto& body : calls)
  {
    EXPECT_EQ(body.find("vaultAddress"), std::string::npos) << body;
  }
  ASSERT_EQ(gSignedActivePools.size(), 3u);
  for (const auto& pool : gSignedActivePools)
  {
    EXPECT_FALSE(pool.has_value());
  }
}

TEST(HyperliquidVaultAddress, EmptyVaultIsTreatedAsNone)
{
  Harness h("hl_vault_empty.log", "https://unused.invalid/exchange", 0, std::string());
  const auto calls = submitModifyCancel(h);

  ASSERT_EQ(calls.size(), 3u);
  for (const auto& body : calls)
  {
    EXPECT_EQ(body.find("vaultAddress"), std::string::npos) << body;
  }
  for (const auto& pool : gSignedActivePools)
  {
    EXPECT_FALSE(pool.has_value());
  }
}

TEST(HyperliquidVaultAddress, LeverageAndPositionTpslCarryTheVault)
{
  Harness h("hl_vault_other.log", "https://unused.invalid/exchange", 0, std::string(kVault));
  h.transport->responses.push_back(R"({"status":"ok","response":{"type":"default"}})");
  h.executor->setLeverage(h.btc, 5, true);
  h.executor->placePosTpsl(h.btc, Side::BUY, Price::fromDouble(58000.0), Price::fromDouble(62000.0),
                           Quantity::fromDouble(0.01), 92, 93);

  ASSERT_EQ(h.transport->calls.size(), 2u);
  for (const auto& body : h.transport->calls)
  {
    EXPECT_EQ(vaultFieldOf(body), kVault) << body;
  }
  ASSERT_EQ(gSignedActivePools.size(), 2u);
  for (const auto& pool : gSignedActivePools)
  {
    ASSERT_TRUE(pool.has_value());
    EXPECT_EQ(*pool, std::string("\"") + kVault + "\"");
  }
}

TEST(HyperliquidAssetMap, MainnetFallbackIsLoggedAsAWarning)
{
  auto owner = std::make_unique<FakeTransport>();
  SymbolRegistry registry;
  OrderTracker tracker;
  auto logger = std::make_shared<CaptureLogger>();
  HyperliquidOrderExecutor executor(std::move(owner), "http://127.0.0.1:9/custom", "aa", &registry,
                                    &tracker, logger, "0xacct", std::nullopt, true);
  const auto lines = logger->lines();
  EXPECT_TRUE(std::any_of(lines.begin(), lines.end(),
                          [](const std::string& l)
                          {
                            return l.find("does not end with /exchange") != std::string::npos &&
                                   l.find("https://api.hyperliquid.xyz/info") != std::string::npos;
                          }));
}

// ---------------------------------------------------------------- precision boundaries

TEST(HyperliquidPrecision, SizeJustBelowAStepIsNeverRoundedUp)
{
  Harness h("hl_sz_boundary.log");
  Order pepe;
  pepe.id = 100;
  pepe.symbol = h.pepe;
  pepe.side = Side::SELL;
  pepe.type = OrderType::LIMIT;
  pepe.price = Price::fromDouble(0.00001);
  pepe.quantity = Quantity::fromDouble(2.9999991);  // szDecimals 0
  pepe.flags.reduceOnly = 1;
  h.executor->submitOrder(pepe);

  Order btc = limitBuy(h.btc, 101);
  btc.quantity = Quantity::fromDouble(0.00001999);  // szDecimals 5
  h.executor->submitOrder(btc);

  Order exact = limitBuy(h.btc, 102);
  exact.quantity = Quantity::fromDouble(1.23);  // on the grid stays as is
  h.executor->submitOrder(exact);

  ASSERT_EQ(h.transport->calls.size(), 3u);
  EXPECT_NE(h.transport->calls[0].find(R"("s":"2",)"), std::string::npos) << h.transport->calls[0];
  EXPECT_NE(h.transport->calls[1].find(R"("s":"0.00001",)"), std::string::npos)
      << h.transport->calls[1];
  EXPECT_NE(h.transport->calls[2].find(R"("s":"1.23",)"), std::string::npos)
      << h.transport->calls[2];
}

TEST(HyperliquidPrecision, TriggerPriceBelow100000KeepsItsDecimals)
{
  Harness h("hl_trig_decimals.log");
  Order o;
  o.id = 103;
  o.symbol = h.eth;
  o.side = Side::SELL;
  o.type = OrderType::STOP_MARKET;
  o.triggerPrice = Price::fromDouble(1234.567);  // 5 sig figs -> 1234.6
  o.quantity = Quantity::fromDouble(0.5);
  o.flags.reduceOnly = 1;
  h.executor->submitOrder(o);

  ASSERT_EQ(h.transport->calls.size(), 1u);
  // p = 1234.567 * 0.9 = 1111.1103 -> 1111.1.
  EXPECT_EQ(maskCloids(actionOf(h.transport->calls[0])),
            R"({"type":"order","orders":[{"a":1,"b":false,"p":"1111.1","s":"0.5","r":true,)"
            R"("t":{"trigger":{"isMarket":true,"triggerPx":"1234.6","tpsl":"sl"}},)"
            R"("c":"CLOID"}],"grouping":"na"})");
}

// ---------------------------------------------------------------- modify target

// A bare "waitingForFill" acknowledges an order without an oid; the cloid is
// then the only handle the venue's modify can take.
TEST(HyperliquidModify, OrderWithoutAnOidIsModifiedByItsCloid)
{
  Harness h("hl_modify_cloid.log");
  h.transport->responses.push_back(
      R"({"status":"ok","response":{"type":"order","data":{"statuses":["waitingForFill"]}}})");
  h.executor->submitOrder(limitBuy(h.btc, 110));
  ASSERT_TRUE(h.tracker.get(110).has_value());
  ASSERT_TRUE(h.tracker.get(110)->exchangeOrderId.empty());
  const std::string cloid = h.tracker.get(110)->clientOrderId;
  ASSERT_FALSE(cloid.empty());

  Order moved = limitBuy(h.btc, 112);
  moved.price = Price::fromDouble(59900.0);
  moved.flags.postOnly = 1;
  h.transport->responses.push_back(R"({"status":"ok","response":{"type":"default"}})");
  h.executor->replaceOrder(110, moved);

  ASSERT_EQ(h.transport->calls.size(), 2u);
  const std::string action = actionOf(h.transport->calls[1]);
  EXPECT_EQ(action.rfind(R"({"type":"modify","oid":")" + cloid + R"(","order":{)", 0), 0u)
      << action;
  EXPECT_NE(action.find(R"("t":{"limit":{"tif":"Alo"}})"), std::string::npos) << action;
  EXPECT_EQ(gSignedActions.back(), action);
}

// HL's modify without always_place takes a non-trigger order only, so a
// trigger replacement is refused before anything is signed or sent.
TEST(HyperliquidModify, TriggerReplacementIsRejectedLocally)
{
  Harness h("hl_modify_trigger.log");
  h.executor->submitOrder(stopMarketSell(h.btc));
  ASSERT_EQ(h.transport->calls.size(), 1u);
  ASSERT_EQ(gSignedActions.size(), 1u);

  Order moved = stopMarketSell(h.btc);
  moved.id = 11;
  moved.triggerPrice = Price::fromDouble(57000.0);
  h.executor->replaceOrder(10, moved);

  EXPECT_EQ(h.transport->calls.size(), 1u);
  EXPECT_EQ(gSignedActions.size(), 1u);
  auto evs = h.events();
  EXPECT_EQ(statusesFor(evs, 11), (std::vector<OrderEventStatus>{OrderEventStatus::REJECTED}));
  EXPECT_TRUE(h.tracker.isActive(10)) << "the resting stop is untouched";
}

TEST(HyperliquidModify, NeitherOidNorCloidRejectsTheReplacement)
{
  Harness h("hl_modify_none.log");
  Order tracked = limitBuy(h.btc, 113);
  ASSERT_TRUE(h.tracker.onSubmitted(tracked, "", ""));

  Order moved = limitBuy(h.btc, 114);
  moved.price = Price::fromDouble(60100.0);
  h.executor->replaceOrder(113, moved);

  EXPECT_TRUE(h.transport->calls.empty());
  EXPECT_TRUE(gSignedActions.empty());
  auto evs = h.events();
  EXPECT_EQ(statusesFor(evs, 114), (std::vector<OrderEventStatus>{OrderEventStatus::REJECTED}));
}

// ---------------------------------------------------------------- leverage outcomes

TEST(HyperliquidLeverage, TransportErrorReachesTheCallback)
{
  Harness h("hl_lev_transport.log");
  h.transport->exchangeError = "connection reset";
  bool called = false;
  bool ok = true;
  std::string detail;
  h.executor->setLeverage(h.btc, 5, true,
                          [&](bool o, const std::string& d)
                          {
                            called = true;
                            ok = o;
                            detail = d;
                          });
  ASSERT_TRUE(called);
  EXPECT_FALSE(ok);
  EXPECT_NE(detail.find("connection reset"), std::string::npos) << detail;
}

// ---------------------------------------------------------------- rate limit

namespace
{
struct RateLimitedHarness
{
  std::unique_ptr<FakeTransport> owner{std::make_unique<FakeTransport>()};
  FakeTransport* transport{owner.get()};
  SymbolRegistry registry;
  OrderTracker tracker;
  SymbolId btc{0};
  OrderExecutionBus bus;
  EventLog log;
  std::unique_ptr<HyperliquidOrderExecutorT<WithRateLimit>> executor;

  explicit RateLimitedHarness(const char* logName, RateLimitPolicy policy)
  {
    gSignedActions.clear();
    gSignedActivePools.clear();
    SymbolInfo info;
    info.exchange = "hyperliquid";
    info.symbol = "BTC";
    info.type = InstrumentType::Future;
    btc = registry.registerSymbol(info);

    RateLimitConfig cfg{};
    cfg.capacity = 1;
    cfg.refillRate = 1;
    cfg.policy = policy;
    executor = std::make_unique<HyperliquidOrderExecutorT<WithRateLimit>>(
        std::move(owner), "https://unused.invalid/exchange", "aa", &registry, &tracker,
        makeLogger(logName), "0xacct", std::nullopt, true, std::move(cfg));
    bus.subscribe(&log);
    bus.start();
    executor->setOrderBus(&bus);
  }

  ~RateLimitedHarness()
  {
    executor.reset();
    bus.stop();
  }

  std::vector<OrderEvent> events()
  {
    bus.flush();
    return log.events();
  }
};
}  // namespace

TEST(HyperliquidRateLimit, RefusedLeverageReachesTheCallback)
{
  RateLimitedHarness h("hl_rl_lev.log", RateLimitPolicy::REJECT);
  h.transport->responses.push_back(R"({"status":"ok","response":{"type":"default"}})");
  h.executor->setLeverage(h.btc, 5, true);  // takes the only token
  ASSERT_EQ(h.transport->calls.size(), 1u);

  bool called = false;
  bool ok = true;
  std::string detail;
  h.executor->setLeverage(h.btc, 6, true,
                          [&](bool o, const std::string& d)
                          {
                            called = true;
                            ok = o;
                            detail = d;
                          });
  EXPECT_EQ(h.transport->calls.size(), 1u);
  ASSERT_TRUE(called);
  EXPECT_FALSE(ok);
  EXPECT_EQ(detail, "client-side rate limit");
}

TEST(HyperliquidRateLimit, RefusedPositionTpslPublishesBothLegs)
{
  RateLimitedHarness h("hl_rl_tpsl.log", RateLimitPolicy::REJECT);
  h.executor->submitOrder(limitBuy(h.btc, 120));  // takes the only token
  ASSERT_EQ(h.transport->calls.size(), 1u);

  h.executor->placePosTpsl(h.btc, Side::BUY, Price::fromDouble(58000.0), Price::fromDouble(65000.0),
                           Quantity::fromDouble(0.01), 121, 122);
  EXPECT_EQ(h.transport->calls.size(), 1u);

  auto evs = h.events();
  const std::vector<OrderEventStatus> refused{OrderEventStatus::REJECTED_RATE_LIMIT};
  EXPECT_EQ(statusesFor(evs, 121), refused);
  EXPECT_EQ(statusesFor(evs, 122), refused);
  for (const auto& e : evs)
  {
    if (e.order.id == 121)
    {
      EXPECT_EQ(e.order.type, OrderType::STOP_MARKET);
      EXPECT_EQ(e.order.side, Side::SELL);
    }
  }
}

namespace
{
// Shared with the test so it outlives the transport the executor owns.
struct TransportProbe
{
  std::atomic<int> posts{0};
  std::atomic<int> postsDuringTeardown{0};
  std::atomic<bool> tearingDown{false};
};

// Answers the meta request, records every other post, and takes its time to
// be destroyed: a request a deferred sender issues while the executor tears
// the transport down lands inside that window.
class SlowTeardownTransport final : public ITransport
{
 public:
  explicit SlowTeardownTransport(std::shared_ptr<TransportProbe> probe) : _probe(std::move(probe))
  {
  }

  ~SlowTeardownTransport() override
  {
    _probe->tearingDown = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
  }

  void post(std::string_view url, std::string_view,
            const std::vector<std::pair<std::string_view, std::string_view>>&,
            MoveOnlyFunction<void(std::string_view)> onSuccess,
            MoveOnlyFunction<void(std::string_view)>) override
  {
    if (url.ends_with("/info"))
    {
      if (onSuccess)
      {
        onSuccess(kMeta);
      }
      return;
    }
    ++_probe->posts;
    if (_probe->tearingDown)
    {
      ++_probe->postsDuringTeardown;
    }
    if (onSuccess)
    {
      onSuccess(
          R"({"status":"ok","response":{"type":"order","data":{"statuses":[{"resting":{"oid":1}}]}}})");
    }
  }

 private:
  std::shared_ptr<TransportProbe> _probe;
};
}  // namespace

// A request the WAIT policy deferred runs on the policy's sender thread. The
// executor stops that thread before it releases the transport, so a token
// that frees up during destruction never sends through a transport that is
// going away; the deferred request is refused instead.
TEST(HyperliquidRateLimit, DestructionWithAQueuedRequestNeverTouchesTheTransport)
{
  auto probe = std::make_shared<TransportProbe>();
  SymbolRegistry registry;
  SymbolInfo info;
  info.exchange = "hyperliquid";
  info.symbol = "BTC";
  info.type = InstrumentType::Future;
  const SymbolId btc = registry.registerSymbol(info);
  OrderTracker tracker;
  OrderExecutionBus bus;
  EventLog log;
  bus.subscribe(&log);
  bus.start();

  RateLimitConfig cfg{};
  cfg.capacity = 1;
  cfg.refillRate = 20;  // the next token is 50 ms away, inside the teardown window
  cfg.policy = RateLimitPolicy::WAIT;
  auto executor = std::make_unique<HyperliquidOrderExecutorT<WithRateLimit>>(
      std::make_unique<SlowTeardownTransport>(probe), "https://unused.invalid/exchange", "aa",
      &registry, &tracker, makeLogger("hl_rl_teardown.log"), "0xacct", std::nullopt, true,
      std::move(cfg));
  executor->setOrderBus(&bus);

  executor->submitOrder(limitBuy(btc, 130));  // takes the only token, sent inline
  executor->submitOrder(limitBuy(btc, 131));  // deferred to the sender thread
  EXPECT_EQ(probe->posts.load(), 1);

  executor.reset();

  EXPECT_EQ(probe->posts.load(), 1);
  EXPECT_EQ(probe->postsDuringTeardown.load(), 0);
  bus.flush();
  EXPECT_EQ(statusesFor(log.events(), 131),
            (std::vector<OrderEventStatus>{OrderEventStatus::REJECTED_RATE_LIMIT}));
  bus.stop();
}
