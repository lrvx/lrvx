/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Every executor path where the venue refuses a cancel, a replace or a
 * position stop, or where the request never reaches the venue, publishes an
 * order event carrying the venue's own error text. These paths used to end in
 * a log line, so the strategy went on believing its cancel or amend had gone
 * through.
 *
 * The refusals are in the venues' documented error envelopes, with the venue's
 * own error texts: Bybit v5 {"retCode":...,"retMsg":...} (110001 "order not
 * exists or too late to cancel", 10001 "params error"), Bitget v2
 * {"code":...,"msg":...} (40768 "Order does not exist"), Hyperliquid
 * {"status":"err","response":...} and per-order
 * {"status":"ok",...,"statuses":[{"error":...}]} ("Order was never placed,
 * already canceled, or filled."). They are answered through the transport the
 * executor already takes, so no request leaves the process.
 */

#include "lrvx-connectors/bitget/authenticated_rest_client.h"
#include "lrvx-connectors/bitget/bitget_order_executor.h"
#include "lrvx-connectors/bybit/authenticated_rest_client.h"
#include "lrvx-connectors/bybit/bybit_order_executor.h"
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

#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace lrvx::hl
{
std::optional<HlSig> hl_sign_with_sdk(const HlSignParams&) { return HlSig{"0xr", "0xs", 27}; }
}  // namespace lrvx::hl

using namespace lrvx;

namespace
{

// Answers each request with the next scripted reply: a body, or a transport
// error when the reply starts with "!".
class ScriptedTransport final : public ITransport
{
 public:
  void post(std::string_view url, std::string_view body,
            const std::vector<std::pair<std::string_view, std::string_view>>&,
            MoveOnlyFunction<void(std::string_view)> onSuccess,
            MoveOnlyFunction<void(std::string_view)> onError) override
  {
    if (url.ends_with("/info"))
    {
      if (onSuccess)
      {
        onSuccess(R"({"universe":[{"szDecimals":5,"name":"BTC"}]})");
      }
      return;
    }
    bodies.emplace_back(body);
    std::string reply = replies.empty() ? fallback : replies.front();
    if (!replies.empty())
    {
      replies.pop_front();
    }
    if (!reply.empty() && reply.front() == '!')
    {
      if (onError)
      {
        onError(reply.substr(1));
      }
      return;
    }
    if (onSuccess)
    {
      onSuccess(reply);
    }
  }

  std::deque<std::string> replies;
  std::string fallback;
  std::vector<std::string> bodies;
};

class Events final : public IOrderExecutionListener
{
 public:
  Events() : IOrderExecutionListener(31) {}
  void onOrderEvent(const OrderEvent& ev) override
  {
    std::lock_guard lock(_m);
    _events.push_back(ev);
  }
  std::vector<OrderEvent> all()
  {
    std::lock_guard lock(_m);
    return _events;
  }

 private:
  std::mutex _m;
  std::vector<OrderEvent> _events;
};

SymbolId registerBtc(SymbolRegistry& registry, const std::string& exchange, const std::string& name)
{
  SymbolInfo info;
  info.exchange = exchange;
  info.symbol = name;
  info.type = InstrumentType::Future;
  info.tickSize = Price::fromDouble(0.1);
  info.tickSizeKnown = true;
  return registry.registerSymbol(info);
}

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

// One executor of each kind over a scripted transport, with the bus wired.
struct BybitRig
{
  ScriptedTransport transport;
  SymbolRegistry registry;
  OrderTracker tracker;
  OrderExecutionBus bus;
  Events events;
  SymbolId sym;
  std::unique_ptr<BybitOrderExecutorT<NoPolicies>> ex;

  BybitRig()
  {
    sym = registerBtc(registry, "bybit", "BTCUSDT");
    transport.fallback = R"({"retCode":0,"retMsg":"OK","result":{"orderId":"EX-1"}})";
    ex = std::make_unique<BybitOrderExecutorT<NoPolicies>>(
        std::make_unique<AuthenticatedRestClient>("k", "s", "https://unused.invalid", &transport),
        &registry, &tracker);
    bus.subscribe(&events);
    bus.start();
    ex->setOrderBus(&bus);
  }
  ~BybitRig() { bus.stop(); }
  std::vector<OrderEvent> drain()
  {
    bus.flush();
    return events.all();
  }
};

struct BitgetRig
{
  ScriptedTransport transport;
  SymbolRegistry registry;
  OrderTracker tracker;
  OrderExecutionBus bus;
  Events events;
  SymbolId sym;
  std::unique_ptr<BitgetOrderExecutorT<NoPolicies>> ex;

  BitgetRig()
  {
    sym = registerBtc(registry, "bitget", "BTCUSDT");
    transport.fallback = R"({"code":"00000","msg":"success","data":{"orderId":"BG-1"}})";
    Bitget::Params p;
    p.productType = "USDT-FUTURES";
    p.marginCoin = "USDT";
    p.marginMode = "crossed";
    ex = std::make_unique<BitgetOrderExecutorT<NoPolicies>>(
        std::make_unique<BitgetAuthenticatedRestClient>("k", "s", "p", "https://unused.invalid",
                                                        &transport),
        &registry, &tracker, p);
    bus.subscribe(&events);
    bus.start();
    ex->setOrderBus(&bus);
  }
  ~BitgetRig() { bus.stop(); }
  std::vector<OrderEvent> drain()
  {
    bus.flush();
    return events.all();
  }
};

std::shared_ptr<AtomicLogger> hlLogger()
{
  auto dir = std::filesystem::temp_directory_path() / "lrvx_failure_event_logs";
  std::filesystem::create_directories(dir);
  AtomicLoggerOptions opts;
  opts.directory = dir.string();
  opts.basename = "hl.log";
  return std::make_shared<AtomicLogger>(opts);
}

struct HlRig
{
  ScriptedTransport* transport;
  SymbolRegistry registry;
  OrderTracker tracker;
  OrderExecutionBus bus;
  Events events;
  SymbolId sym;
  std::unique_ptr<HyperliquidOrderExecutorT<NoPolicies>> ex;

  HlRig()
  {
    sym = registerBtc(registry, "hyperliquid", "BTC");
    auto owned = std::make_unique<ScriptedTransport>();
    transport = owned.get();
    transport->fallback =
        R"({"status":"ok","response":{"type":"order","data":{"statuses":[{"resting":{"oid":777}}]}}})";
    ex = std::make_unique<HyperliquidOrderExecutorT<NoPolicies>>(
        std::move(owned), "https://unused.invalid", "aa", &registry, &tracker, hlLogger(), "0xacct",
        std::nullopt, true);
    bus.subscribe(&events);
    bus.start();
    ex->setOrderBus(&bus);
  }
  ~HlRig() { bus.stop(); }
  std::vector<OrderEvent> drain()
  {
    bus.flush();
    return events.all();
  }
};

void expectOne(const std::vector<OrderEvent>& evs, OrderEventStatus status, OrderId id,
               const std::string& reasonPart)
{
  ASSERT_EQ(evs.size(), 1u) << "expected exactly one event";
  EXPECT_EQ(evs[0].status, status);
  EXPECT_EQ(evs[0].order.id, id);
  EXPECT_NE(evs[0].rejectReason.find(reasonPart), std::string::npos) << evs[0].rejectReason;
}

}  // namespace

// ------------------------------------------------------------------ Bybit

TEST(FailureEventsBybit, CancelRefusedByTheVenue)
{
  BybitRig r;
  r.ex->submitOrder(limitBuy(1, r.sym, 60000.1));
  r.transport.replies.push_back(
      R"({"retCode":110001,"retMsg":"order not exists or too late to cancel","result":{},"retExtInfo":{},"time":1794481815623})");
  r.ex->cancelOrder(1);
  expectOne(r.drain(), OrderEventStatus::REJECTED, 1, "order not exists or too late to cancel");
  EXPECT_TRUE(r.tracker.isActive(1)) << "a refused cancel leaves the order live";
}

TEST(FailureEventsBybit, CancelTransportError)
{
  BybitRig r;
  r.ex->submitOrder(limitBuy(1, r.sym, 60000.1));
  r.transport.replies.push_back("!HTTP 502: Bad Gateway");
  r.ex->cancelOrder(1);
  expectOne(r.drain(), OrderEventStatus::REJECTED, 1, "HTTP 502");
}

TEST(FailureEventsBybit, AmendRefusedByTheVenue)
{
  BybitRig r;
  r.ex->submitOrder(limitBuy(1, r.sym, 60000.1));
  r.transport.replies.push_back(
      R"({"retCode":10001,"retMsg":"params error: price invalid","result":{},"retExtInfo":{},"time":1794481815623})");
  r.ex->replaceOrder(1, limitBuy(2, r.sym, 60010.2));
  const auto evs = r.drain();
  expectOne(evs, OrderEventStatus::REPLACE_REJECTED, 1, "params error: price invalid");
  EXPECT_EQ(evs[0].newOrder.id, 2u);
  EXPECT_TRUE(r.tracker.isActive(1));
  EXPECT_FALSE(r.tracker.exists(2));
}

TEST(FailureEventsBybit, AmendTransportError)
{
  BybitRig r;
  r.ex->submitOrder(limitBuy(1, r.sym, 60000.1));
  r.transport.replies.push_back("!Operation timed out");
  r.ex->replaceOrder(1, limitBuy(2, r.sym, 60010.2));
  expectOne(r.drain(), OrderEventStatus::REPLACE_REJECTED, 1, "Operation timed out");
}

TEST(FailureEventsBybit, AmendOffTheTickIsRefusedLocally)
{
  BybitRig r;
  r.ex->submitOrder(limitBuy(1, r.sym, 60000.1));
  r.ex->replaceOrder(1, limitBuy(2, r.sym, 60010.25));
  expectOne(r.drain(), OrderEventStatus::REPLACE_REJECTED, 1, "price_off_tick");
  EXPECT_EQ(r.transport.bodies.size(), 1u) << "the amend must not be sent";
}

// A body the venue (or a proxy in front of it) did not shape as v5 used to
// throw inside the response callback; the transport swallowed the exception
// and the order was left with no event at all.
TEST(FailureEventsBybit, UnparseableSubmitAnswerIsARejection)
{
  BybitRig r;
  r.transport.replies.push_back("<html>502 Bad Gateway</html>");
  r.ex->submitOrder(limitBuy(1, r.sym, 60000.1));
  expectOne(r.drain(), OrderEventStatus::REJECTED, 1, "unparseable response");
}

// ------------------------------------------------------------------ Bitget

TEST(FailureEventsBitget, CancelRefusedByTheVenue)
{
  BitgetRig r;
  r.ex->submitOrder(limitBuy(1, r.sym, 60000.1));
  r.transport.replies.push_back(
      R"({"code":"40768","msg":"Order does not exist","requestTime":1794481815778,"data":null})");
  r.ex->cancelOrder(1);
  expectOne(r.drain(), OrderEventStatus::REJECTED, 1, "Order does not exist");
  EXPECT_TRUE(r.tracker.isActive(1));
}

TEST(FailureEventsBitget, CancelTransportErrorCarriesTheVenueBody)
{
  BitgetRig r;
  r.ex->submitOrder(limitBuy(1, r.sym, 60000.1));
  // Bitget answers most refusals with HTTP 400; the transport hands over the
  // status and the body.
  r.transport.replies.push_back(
      R"(!HTTP 400: {"code":"40768","msg":"Order does not exist","requestTime":1794481815778,"data":null})");
  r.ex->cancelOrder(1);
  expectOne(r.drain(), OrderEventStatus::REJECTED, 1, "Order does not exist");
}

TEST(FailureEventsBitget, ReplaceRefusedByTheVenue)
{
  BitgetRig r;
  r.ex->submitOrder(limitBuy(1, r.sym, 60000.1));
  r.transport.replies.push_back(
      R"({"code":"40768","msg":"Order does not exist","requestTime":1794481815778,"data":null})");
  r.ex->replaceOrder(1, limitBuy(2, r.sym, 60010.2));
  const auto evs = r.drain();
  expectOne(evs, OrderEventStatus::REPLACE_REJECTED, 1, "Order does not exist");
  EXPECT_EQ(evs[0].newOrder.id, 2u);
  EXPECT_TRUE(r.tracker.isActive(1));
}

TEST(FailureEventsBitget, ReplaceTransportError)
{
  BitgetRig r;
  r.ex->submitOrder(limitBuy(1, r.sym, 60000.1));
  r.transport.replies.push_back("!Couldn't connect to server");
  r.ex->replaceOrder(1, limitBuy(2, r.sym, 60010.2));
  expectOne(r.drain(), OrderEventStatus::REPLACE_REJECTED, 1, "Couldn't connect to server");
}

TEST(FailureEventsBitget, PositionStopRefusedByTheVenue)
{
  BitgetRig r;
  r.transport.replies.push_back(
      R"({"code":"40915","msg":"The stop loss price must be lower than the current price","requestTime":1794481815778,"data":null})");
  r.ex->placePosTpsl(r.sym, HoldSide::Long, 58000.1, 0.0, 7);
  expectOne(r.drain(), OrderEventStatus::REJECTED, 7,
            "The stop loss price must be lower than the current price");
}

TEST(FailureEventsBitget, PositionStopTransportError)
{
  BitgetRig r;
  r.transport.replies.push_back("!Operation timed out");
  r.ex->placePosTpsl(r.sym, HoldSide::Long, 58000.1, 0.0, 7);
  expectOne(r.drain(), OrderEventStatus::REJECTED, 7, "Operation timed out");
}

TEST(FailureEventsBitget, PositionStopModifyRefusedReachesTheTrackedOrder)
{
  BitgetRig r;
  r.transport.replies.push_back(
      R"({"code":"00000","msg":"success","data":[{"orderId":"TPSL-9"}]})");
  r.ex->placePosTpsl(r.sym, HoldSide::Long, 58000.1, 0.0, 7);
  ASSERT_TRUE(r.tracker.exists(7));
  r.transport.replies.push_back(
      R"({"code":"40768","msg":"Order does not exist","requestTime":1794481815778,"data":null})");
  r.ex->modifyPosTpsl(r.sym, "TPSL-9", 58100.2, 1.0);
  const auto evs = r.drain();
  expectOne(evs, OrderEventStatus::REPLACE_REJECTED, 7, "Order does not exist");
  EXPECT_EQ(evs[0].newOrder.triggerPrice.raw(), Price::fromDouble(58100.2).raw());
}

TEST(FailureEventsBitget, PositionStopModifyTransportError)
{
  BitgetRig r;
  r.transport.replies.push_back(
      R"({"code":"00000","msg":"success","data":[{"orderId":"TPSL-9"}]})");
  r.ex->placePosTpsl(r.sym, HoldSide::Long, 58000.1, 0.0, 7);
  r.transport.replies.push_back("!Operation timed out");
  r.ex->modifyPosTpsl(r.sym, "TPSL-9", 58100.2, 1.0);
  expectOne(r.drain(), OrderEventStatus::REPLACE_REJECTED, 7, "Operation timed out");
}

TEST(FailureEventsBitget, UnparseableSubmitAnswerIsARejection)
{
  BitgetRig r;
  r.transport.replies.push_back("<html>502 Bad Gateway</html>");
  r.ex->submitOrder(limitBuy(1, r.sym, 60000.1));
  expectOne(r.drain(), OrderEventStatus::REJECTED, 1, "unparseable response");
}

// ------------------------------------------------------------------ Hyperliquid

TEST(FailureEventsHyperliquid, CancelOfAnOrderTheVenueNoLongerHas)
{
  HlRig r;
  r.ex->submitOrder(limitBuy(1, r.sym, 60000.0));
  // Top-level "ok", per-order error: the cancel did not happen.
  r.transport->replies.push_back(
      R"({"status":"ok","response":{"type":"cancel","data":{"statuses":[{"error":"Order was never placed, already canceled, or filled."}]}}})");
  r.ex->cancelOrder(1);
  auto evs = r.drain();
  ASSERT_GE(evs.size(), 2u);  // SUBMITTED, then the refusal
  evs.erase(evs.begin());
  expectOne(evs, OrderEventStatus::REJECTED, 1, "Order was never placed");
  EXPECT_TRUE(r.tracker.isActive(1)) << "the tracker must not mark it canceled";
}

TEST(FailureEventsHyperliquid, CancelRefusedAsAWhole)
{
  HlRig r;
  r.ex->submitOrder(limitBuy(1, r.sym, 60000.0));
  r.transport->replies.push_back(
      R"({"status":"err","response":"User or API Wallet does not exist."})");
  r.ex->cancelOrder(1);
  auto evs = r.drain();
  evs.erase(evs.begin());
  expectOne(evs, OrderEventStatus::REJECTED, 1, "User or API Wallet does not exist.");
}

TEST(FailureEventsHyperliquid, CancelTransportError)
{
  HlRig r;
  r.ex->submitOrder(limitBuy(1, r.sym, 60000.0));
  r.transport->replies.push_back("!connection reset");
  r.ex->cancelOrder(1);
  auto evs = r.drain();
  evs.erase(evs.begin());
  expectOne(evs, OrderEventStatus::REJECTED, 1, "connection reset");
}

TEST(FailureEventsHyperliquid, ModifyRefusedByTheVenue)
{
  HlRig r;
  r.ex->submitOrder(limitBuy(1, r.sym, 60000.0));
  r.transport->replies.push_back(
      R"({"status":"err","response":"Cannot modify canceled or filled order"})");
  r.ex->replaceOrder(1, limitBuy(2, r.sym, 60010.0));
  auto evs = r.drain();
  evs.erase(evs.begin());
  expectOne(evs, OrderEventStatus::REPLACE_REJECTED, 1, "Cannot modify canceled or filled order");
  EXPECT_EQ(evs[0].newOrder.id, 2u);
  EXPECT_FALSE(r.tracker.exists(2));
}

TEST(FailureEventsHyperliquid, ModifyTransportError)
{
  HlRig r;
  r.ex->submitOrder(limitBuy(1, r.sym, 60000.0));
  r.transport->replies.push_back("!connection reset");
  r.ex->replaceOrder(1, limitBuy(2, r.sym, 60010.0));
  auto evs = r.drain();
  evs.erase(evs.begin());
  expectOne(evs, OrderEventStatus::REPLACE_REJECTED, 1, "connection reset");
}

TEST(FailureEventsHyperliquid, ModifyThatCannotBeBuiltIsReported)
{
  HlRig r;
  r.ex->submitOrder(limitBuy(1, r.sym, 60000.0));
  r.ex->replaceOrder(1, limitBuy(2, r.sym, 60000.55));  // seven significant figures
  auto evs = r.drain();
  evs.erase(evs.begin());
  expectOne(evs, OrderEventStatus::REPLACE_REJECTED, 1, "price_off_tick");
  EXPECT_EQ(r.transport->bodies.size(), 1u);
}
