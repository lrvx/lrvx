/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2026 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Offline order-serialization tests for the two ways the Bitget executor
 * loses information on its conditional-order paths:
 *
 *  - submitPlanOrder writes "orderType":"market" unconditionally and never
 *    sends a price, so STOP_LIMIT and TAKE_PROFIT_LIMIT reach the venue as
 *    stop-market. The strategy asked for a bounded execution price and got
 *    an unbounded one, with no rejection and no event to say so --
 *    docs/explanation/connectors.md is explicit that an unsupported order
 *    must be rejected rather than sent as an approximation.
 *
 *  - every trigger price goes through trimDouble() with its default of one
 *    fractional digit, an assumption its own comment spells out ("BTC perp
 *    = 1 digit"). The instrument's precision is in the registry the
 *    executor already holds (SymbolInfo::tickSize); on anything finer than
 *    0.1 the protective trigger is silently moved, and on a symbol priced
 *    below ~0.05 it is sent as "0".
 *
 * Both are asserted on the exact bytes handed to the transport, because
 * that string is the entire contract with the venue.
 */

#include "lrvx-connectors/bitget/authenticated_rest_client.h"
#include "lrvx-connectors/bitget/bitget_order_executor.h"

#include <lrvx/common.h>
#include <lrvx/engine/symbol_registry.h>
#include <lrvx/execution/abstract_execution_listener.h>
#include <lrvx/execution/bus/order_execution_bus.h>
#include <lrvx/execution/events/order_event.h>
#include <lrvx/execution/order.h>
#include <lrvx/execution/order_tracker.h>

#include <gtest/gtest.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace lrvx;

namespace
{

struct Call
{
  std::string url;
  std::string body;
};

class FakeTransport final : public ITransport
{
 public:
  void post(std::string_view url, std::string_view body,
            const std::vector<std::pair<std::string_view, std::string_view>>&,
            MoveOnlyFunction<void(std::string_view)> onSuccess,
            MoveOnlyFunction<void(std::string_view)>) override
  {
    calls.push_back({std::string(url), std::string(body)});
    if (onSuccess)
    {
      onSuccess(nextResponse);
    }
  }

  std::vector<Call> calls;
  std::string nextResponse = R"({"code":"00000","msg":"success","data":{"orderId":"BG-1"}})";
};

// The pos-tpsl endpoint answers with an array, not an object.
constexpr const char* kPosTpslResponse =
    R"({"code":"00000","msg":"success","data":[{"orderId":"BG-TPSL-1"}]})";

Bitget::Params baseParams()
{
  Bitget::Params p;
  p.productType = "USDT-FUTURES";
  p.marginCoin = "USDT";
  p.marginMode = "crossed";
  p.positionMode = Bitget::PositionMode::OneWay;
  return p;
}

SymbolId registerSymbol(SymbolRegistry& registry, const std::string& symbol, double tickSize)
{
  SymbolInfo info;
  info.exchange = "bitget";
  info.symbol = symbol;
  info.type = InstrumentType::Future;
  info.tickSize = Price::fromDouble(tickSize);
  return registry.registerSymbol(info);
}

// A venue whose instrument listing carried no usable tick size. Rounding to
// one digit here is a guess about an instrument nothing knows anything about;
// the honest fallback is everything the fixed-point price type can hold.
SymbolId registerSymbolWithoutTick(SymbolRegistry& registry, const std::string& symbol)
{
  SymbolInfo info;
  info.exchange = "bitget";
  info.symbol = symbol;
  info.type = InstrumentType::Future;
  info.tickSize = Price::fromRaw(0);
  return registry.registerSymbol(info);
}

// Records the rejections the executor publishes, so a conditional order that
// cannot be represented can be seen to be refused rather than approximated.
class RejectionWatcher final : public IOrderExecutionListener
{
 public:
  RejectionWatcher() : IOrderExecutionListener(13) {}

  struct Rejection
  {
    OrderId id;
    OrderEventStatus status;
    std::string reason;
  };

  void onOrderEvent(const OrderEvent& ev) override
  {
    if (ev.status != OrderEventStatus::REJECTED &&
        ev.status != OrderEventStatus::REJECTED_RATE_LIMIT)
    {
      return;
    }
    std::lock_guard<std::mutex> lk(_m);
    _rejected.push_back({ev.order.id, ev.status, ev.rejectReason});
  }

  std::vector<Rejection> rejected()
  {
    std::lock_guard<std::mutex> lk(_m);
    return _rejected;
  }

 private:
  std::mutex _m;
  std::vector<Rejection> _rejected;
};

struct Harness
{
  std::unique_ptr<FakeTransport> owner{std::make_unique<FakeTransport>()};
  FakeTransport* transport{owner.get()};
  SymbolRegistry registry;
  OrderTracker tracker;
  RejectionWatcher watcher;
  OrderExecutionBus bus;
  std::unique_ptr<BitgetOrderExecutorT<NoPolicies>> executor;

  Harness()
  {
    auto client = std::make_unique<BitgetAuthenticatedRestClient>(
        "k", "s", "p", "https://api.bitget.com", transport);
    executor = std::make_unique<BitgetOrderExecutorT<NoPolicies>>(std::move(client), &registry,
                                                                  &tracker, baseParams());
    bus.subscribe(&watcher);
    bus.start();
    executor->setOrderBus(&bus);
  }

  ~Harness() { bus.stop(); }
};

}  // namespace

// A STOP_LIMIT is a stop with a limit price. The limit price is the whole
// difference between it and a STOP_MARKET, and it is the leg that bounds
// slippage on a protective exit.
TEST(BitgetPlanOrderPrice, StopLimitCarriesItsLimitPrice)
{
  Harness h;
  SymbolId sym = registerSymbol(h.registry, "BTCUSDT", 0.1);

  Order order;
  order.id = 11;
  order.symbol = sym;
  order.side = Side::SELL;
  order.type = OrderType::STOP_LIMIT;
  order.quantity = Quantity::fromDouble(1.0);
  order.triggerPrice = Price::fromDouble(58000.0);
  order.price = Price::fromDouble(57900.0);
  order.flags.reduceOnly = 1;

  h.executor->submitOrder(order);

  ASSERT_EQ(h.transport->calls.size(), 1u);
  const std::string& body = h.transport->calls[0].body;

  EXPECT_NE(body.find(R"("planType":"normal_plan")"), std::string::npos)
      << "the plan type must be the one that carries an execution price: " << body;
  // The whole field, not a prefix: "58000.000000" starts the same way, and
  // that is exactly what a trigger formatted through Price::toString() rather
  // than the instrument's own precision looks like.
  EXPECT_NE(body.find(R"("triggerPrice":"58000")"), std::string::npos) << body;
  EXPECT_NE(body.find(R"("orderType":"limit")"), std::string::npos)
      << "a STOP_LIMIT sent as market is a silent downgrade to stop-market: " << body;
  EXPECT_NE(body.find(R"("price":"57900)"), std::string::npos)
      << "the limit price never reached the venue: " << body;
}

TEST(BitgetPlanOrderPrice, TakeProfitLimitCarriesItsLimitPrice)
{
  Harness h;
  SymbolId sym = registerSymbol(h.registry, "BTCUSDT", 0.1);

  Order order;
  order.id = 12;
  order.symbol = sym;
  order.side = Side::SELL;
  order.type = OrderType::TAKE_PROFIT_LIMIT;
  order.quantity = Quantity::fromDouble(2.0);
  order.triggerPrice = Price::fromDouble(71000.0);
  order.price = Price::fromDouble(71100.0);
  order.flags.reduceOnly = 1;

  h.executor->submitOrder(order);

  ASSERT_EQ(h.transport->calls.size(), 1u);
  const std::string& body = h.transport->calls[0].body;

  EXPECT_NE(body.find(R"("orderType":"limit")"), std::string::npos) << body;
  EXPECT_NE(body.find(R"("price":"71100)"), std::string::npos) << body;
}

// Control, green today: the market variants have no limit price and must
// keep going out as market with no price field. Whatever carries the limit
// price for STOP_LIMIT must not leak into these.
TEST(BitgetPlanOrderPrice, StopMarketStaysMarketWithNoPrice)
{
  Harness h;
  SymbolId sym = registerSymbol(h.registry, "BTCUSDT", 0.1);

  Order order;
  order.id = 13;
  order.symbol = sym;
  order.side = Side::SELL;
  order.type = OrderType::STOP_MARKET;
  order.quantity = Quantity::fromDouble(1.0);
  order.triggerPrice = Price::fromDouble(58000.0);
  order.flags.reduceOnly = 1;

  h.executor->submitOrder(order);

  ASSERT_EQ(h.transport->calls.size(), 1u);
  const std::string& body = h.transport->calls[0].body;
  EXPECT_NE(body.find(R"("orderType":"market")"), std::string::npos) << body;
  EXPECT_EQ(body.find(R"("price":")"), std::string::npos)
      << "a stop-market has no limit price to send: " << body;
}

// The registry already knows this instrument quotes to 1e-8. Rounding the
// trigger to one digit moves a protective stop by up to 0.05 of quote
// currency and, on a venue that validates against the symbol's precision,
// is a different price from the one the strategy computed.
TEST(BitgetPlanOrderPrice, PosTpslTriggerKeepsInstrumentPrecision)
{
  Harness h;
  h.transport->nextResponse = kPosTpslResponse;
  SymbolId sym = registerSymbol(h.registry, "PEPEUSDT", 0.00000001);

  h.executor->placePosTpsl(sym, HoldSide::Long, 67123.45678901, 0.0, 21);

  ASSERT_EQ(h.transport->calls.size(), 1u);
  const std::string& body = h.transport->calls[0].body;
  EXPECT_NE(body.find(R"("stopLossTriggerPrice":"67123.45678901")"), std::string::npos)
      << "the trigger was reformatted at a precision the instrument does not use: " << body;
}

// The same rounding on a symbol whose price lives below the rounding
// granularity destroys the trigger outright rather than nudging it.
TEST(BitgetPlanOrderPrice, PosTpslTriggerSurvivesOnALowPricedSymbol)
{
  Harness h;
  h.transport->nextResponse = kPosTpslResponse;
  SymbolId sym = registerSymbol(h.registry, "DOGEUSDT", 0.0001);

  h.executor->placePosTpsl(sym, HoldSide::Short, 0.0842, 0.0, 22);

  ASSERT_EQ(h.transport->calls.size(), 1u);
  const std::string& body = h.transport->calls[0].body;
  EXPECT_NE(body.find(R"("stopLossTriggerPrice":"0.0842")"), std::string::npos)
      << "a trigger below the hardcoded 0.1 granularity is rounded away entirely: " << body;
}

// A trigger off the instrument's tick is refused with the simulator's reason,
// not rounded: 0.08423 used to go out as 0.0842, a stop the strategy did not
// place, and for this short's stop-loss (a buy) one that fires earlier.
TEST(BitgetPlanOrderPrice, PosTpslTriggerOffTheTickIsRefusedNotRounded)
{
  Harness h;
  h.transport->nextResponse = kPosTpslResponse;
  SymbolId sym = registerSymbol(h.registry, "DOGEUSDT", 0.0001);

  h.executor->placePosTpsl(sym, HoldSide::Short, 0.08423, 0.0, 26);
  h.bus.flush();

  EXPECT_EQ(h.transport->calls.size(), 0u)
      << (h.transport->calls.empty() ? std::string{} : h.transport->calls[0].body);
  const auto rejected = h.watcher.rejected();
  ASSERT_EQ(rejected.size(), 1u);
  EXPECT_EQ(rejected[0].id, 26u);
  EXPECT_EQ(rejected[0].reason, "trigger_off_tick");
}

TEST(BitgetPlanOrderPrice, ModifyPosTpslTriggerKeepsInstrumentPrecision)
{
  Harness h;
  SymbolId sym = registerSymbol(h.registry, "PEPEUSDT", 0.00000001);

  h.executor->modifyPosTpsl(sym, "BG-TPSL-1", 67123.45678901, 1.0);

  ASSERT_EQ(h.transport->calls.size(), 1u);
  const std::string& body = h.transport->calls[0].body;
  EXPECT_NE(body.find(R"("triggerPrice":"67123.45678901")"), std::string::npos)
      << "walking a stop to a price the instrument supports must not round it: " << body;
}

// Control: on an instrument that really does quote to 0.1 an on-tick trigger
// goes out at that precision, unchanged.
TEST(BitgetPlanOrderPrice, PosTpslTriggerOnACoarseInstrumentIsUnchanged)
{
  Harness h;
  h.transport->nextResponse = kPosTpslResponse;
  SymbolId sym = registerSymbol(h.registry, "BTCUSDT", 0.1);

  h.executor->placePosTpsl(sym, HoldSide::Long, 67123.5, 0.0, 23);

  ASSERT_EQ(h.transport->calls.size(), 1u);
  const std::string& body = h.transport->calls[0].body;
  EXPECT_NE(body.find(R"("stopLossTriggerPrice":"67123.5")"), std::string::npos) << body;
}

// The same trigger with eight decimals on that instrument used to be rounded
// to 67123.5; it is refused with trigger_off_tick instead.
TEST(BitgetPlanOrderPrice, PosTpslTriggerOffACoarseTickIsRefused)
{
  Harness h;
  h.transport->nextResponse = kPosTpslResponse;
  SymbolId sym = registerSymbol(h.registry, "BTCUSDT", 0.1);

  h.executor->placePosTpsl(sym, HoldSide::Long, 67123.45678901, 0.0, 27);
  h.bus.flush();

  EXPECT_EQ(h.transport->calls.size(), 0u);
  const auto rejected = h.watcher.rejected();
  ASSERT_EQ(rejected.size(), 1u);
  EXPECT_EQ(rejected[0].reason, "trigger_off_tick");
}

// A conditional limit order with no limit price cannot be represented: the
// limit price is the field that distinguishes it from the market variant.
// Standing the trigger price in for it hands the venue a bound the strategy
// never chose, on an order whose whole purpose is to bound the fill --
// docs/explanation/connectors.md says such an order is rejected rather than
// sent as an approximation.
TEST(BitgetPlanOrderPrice, StopLimitWithoutALimitPriceIsRejected)
{
  Harness h;
  SymbolId sym = registerSymbol(h.registry, "BTCUSDT", 0.1);

  Order order;
  order.id = 31;
  order.symbol = sym;
  order.side = Side::SELL;
  order.type = OrderType::STOP_LIMIT;
  order.quantity = Quantity::fromDouble(1.0);
  order.triggerPrice = Price::fromDouble(58000.0);
  // No limit price at all.
  order.flags.reduceOnly = 1;

  h.executor->submitOrder(order);
  h.bus.flush();

  EXPECT_EQ(h.transport->calls.size(), 0u)
      << "an unrepresentable conditional limit order was sent anyway: "
      << (h.transport->calls.empty() ? std::string{} : h.transport->calls[0].body);

  const auto rejected = h.watcher.rejected();
  ASSERT_EQ(rejected.size(), 1u) << "the order was neither sent nor rejected";
  EXPECT_EQ(rejected[0].id, 31u);
  EXPECT_EQ(rejected[0].status, OrderEventStatus::REJECTED);
  EXPECT_NE(rejected[0].reason.find("limit price"), std::string::npos) << rejected[0].reason;
}

TEST(BitgetPlanOrderPrice, TakeProfitLimitWithoutALimitPriceIsRejected)
{
  Harness h;
  SymbolId sym = registerSymbol(h.registry, "BTCUSDT", 0.1);

  Order order;
  order.id = 32;
  order.symbol = sym;
  order.side = Side::SELL;
  order.type = OrderType::TAKE_PROFIT_LIMIT;
  order.quantity = Quantity::fromDouble(1.0);
  order.triggerPrice = Price::fromDouble(71000.0);
  order.flags.reduceOnly = 1;

  h.executor->submitOrder(order);
  h.bus.flush();

  EXPECT_EQ(h.transport->calls.size(), 0u)
      << "an unrepresentable conditional limit order was sent anyway: "
      << (h.transport->calls.empty() ? std::string{} : h.transport->calls[0].body);

  const auto rejected = h.watcher.rejected();
  ASSERT_EQ(rejected.size(), 1u) << "the order was neither sent nor rejected";
  EXPECT_EQ(rejected[0].id, 32u);
}

// The take-profit leg of a position-attached stop is formatted by the same
// rule as the stop-loss leg. Only the stop-loss leg was ever exercised, so
// the take-profit one could quietly keep the old single digit.
TEST(BitgetPlanOrderPrice, PosTpslTakeProfitKeepsInstrumentPrecision)
{
  Harness h;
  h.transport->nextResponse = kPosTpslResponse;
  SymbolId sym = registerSymbol(h.registry, "PEPEUSDT", 0.00000001);

  h.executor->placePosTpsl(sym, HoldSide::Long, 66000.12345678, 67123.45678901, 24);

  ASSERT_EQ(h.transport->calls.size(), 1u);
  const std::string& body = h.transport->calls[0].body;
  EXPECT_NE(body.find(R"("stopSurplusTriggerPrice":"67123.45678901")"), std::string::npos)
      << "the take-profit trigger was rounded at a precision the instrument does not use: " << body;
  EXPECT_NE(body.find(R"("stopLossTriggerPrice":"66000.12345678")"), std::string::npos) << body;
  EXPECT_NE(body.find(R"("planType":"profit_loss")"), std::string::npos) << body;
}

// An instrument the registry has no tick size for. One digit is a guess about
// something nothing knows anything about, and it is the guess that silently
// rounds a strategy's price away; the fixed-point type's own precision loses
// nothing the strategy asked for.
TEST(BitgetPlanOrderPrice, UnsetTickKeepsFullFixedPointPrecision)
{
  Harness h;
  h.transport->nextResponse = kPosTpslResponse;
  SymbolId sym = registerSymbolWithoutTick(h.registry, "NEWUSDT");

  h.executor->placePosTpsl(sym, HoldSide::Long, 67123.45678901, 0.0, 25);

  ASSERT_EQ(h.transport->calls.size(), 1u);
  const std::string& body = h.transport->calls[0].body;
  EXPECT_NE(body.find(R"("stopLossTriggerPrice":"67123.45678901")"), std::string::npos)
      << "an instrument with no registered tick fell back to a hardcoded digit count: " << body;
}

// Every live symbol used to be registered with the registry's 0.01
// placeholder, and the plan path rounded triggers to the placeholder's two
// digits: a DOGE stop at 0.08423 went out as 0.08. With the tick unknown the
// trigger now goes out exactly as given; the venue applies its own rule.
TEST(BitgetPlanOrderPrice, PlaceholderTickDoesNotRoundAPlanTrigger)
{
  Harness h;
  SymbolInfo info;
  info.exchange = "bitget";
  info.symbol = "DOGEUSDT";
  info.type = InstrumentType::Future;
  const SymbolId sym = h.registry.registerSymbol(info);

  Order order;
  order.id = 41;
  order.symbol = sym;
  order.side = Side::SELL;
  order.type = OrderType::STOP_MARKET;
  order.quantity = Quantity::fromDouble(100.0);
  order.triggerPrice = Price::fromDouble(0.08423);
  order.flags.reduceOnly = 1;
  h.executor->submitOrder(order);

  ASSERT_EQ(h.transport->calls.size(), 1u);
  const std::string& body = h.transport->calls[0].body;
  EXPECT_NE(body.find(R"("triggerPrice":"0.08423")"), std::string::npos) << body;
}
